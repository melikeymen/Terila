#pragma once

#include "log/logger.hpp"
#include "world/chunk.hpp"
#include "world/flat_generator.hpp"
#include "world/terrain_generator.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace terila::world {

    enum class WorldType {
        Terrain,   // heightmap over noise: oceans, hills, biomes and trees
        Flat       // classic superflat: bedrock, three dirt layers and grass
    };

    // server.properties level-type: "normal" keeps the terrain generator, "flat" the
    // superflat one. Anything else is reported so the caller can fall back.
    inline std::optional<WorldType> parse_world_type(std::string_view name) {
        if (name == "flat" || name == "superflat") {
            return WorldType::Flat;
        }
        if (name == "normal" || name == "default" || name == "terrain") {
            return WorldType::Terrain;
        }
        return std::nullopt;
    }

    inline std::string_view world_type_name(WorldType type) {
        return type == WorldType::Flat ? "flat" : "normal";
    }

    // Both generators behind one interface, so level-type decides what a connection
    // streams without the chunk, block and mob code knowing which one is active.
    class WorldGenerator {
    public:
        bool init(WorldType type, int min_y, int section_count, std::uint32_t seed,
            const terrain::IdResolver& block, const terrain::IdResolver& biome,
            const worldgen::StateTextResolver& state = {},
            worldgen::DimensionKind kind = worldgen::DimensionKind::Overworld,
            const worldgen::StateRotator& rotate = {},
            const LightTable* light_table = nullptr) {
            type_ = type;
            kind_ = kind;
            min_y_ = min_y;
            section_count_ = section_count;
            seed_ = seed;
            block_ = block;
            biome_ = biome;

            if (type_ != WorldType::Flat) {
                ready_ = terrain_.init(min_y, section_count, seed, block, biome, state, kind,
                    rotate, light_table);
                return ready_;
            }

            spec_.min_y = min_y;
            spec_.section_count = section_count;

            air_ = block(spec_.air).value_or(0);
            bedrock_ = block(spec_.bedrock).value_or(0);
            dirt_ = block(spec_.dirt).value_or(0);
            grass_ = block(spec_.grass).value_or(0);

            // Air is the only thing the flat world lets light through.
            flat_rules_ = LightRules{};
            flat_rules_.set(air_, LightBlock{ 0, 0, true });
            terrain::add_placed_light_sources(flat_rules_, block);

            ready_ = block(spec_.air).has_value() && block(spec_.bedrock).has_value() &&
                block(spec_.dirt).has_value() && block(spec_.grass).has_value() &&
                biome(spec_.biome).has_value();
            if (!ready_) {
                LOG_WARN(Chunk, "flat world needs {}, {}, {} and biome {} in the registry",
                    spec_.bedrock, spec_.dirt, spec_.grass, spec_.biome);
            }
            return ready_;
        }

        bool ready() const { return ready_; }
        const LightRules& light_rules() const {
            return type_ == WorldType::Flat ? flat_rules_ : terrain_.light_rules();
        }
        WorldType type() const { return type_; }
        worldgen::DimensionKind kind() const { return kind_; }
        std::uint32_t seed() const { return seed_; }
        int min_y() const { return min_y_; }
        int section_count() const { return section_count_; }

        int sea_level() const {
            return type_ == WorldType::Flat ? kSeaLevel : terrain_.sea_level();
        }

        // Highest solid block of a column, water and plants excluded.
        int surface_height(std::int32_t x, std::int32_t z) const {
            if (type_ == WorldType::Flat) {
                return min_y_ + spec_.dirt_layers + 1;
            }
            return terrain_.surface_height(x, z);
        }

        // Index into the biome table for a column: the spawn lists hang off the biome, so
        // natural spawning reads it through here. A flat world is plains.
        int biome_at(std::int32_t x, std::int32_t z) const {
            if (type_ == WorldType::Flat) {
                return terrain::kPlains;
            }
            return terrain_.biome_at(x, z);
        }

        struct SpawnPoint {
            std::int32_t x = 8;
            std::int32_t z = 8;
            int surface_y = 0;
        };

        // A fixed (8, 8) lands on whatever the seed produces there, which on a mountainous
        // seed is a peak. Walk a coarse grid instead and take the dry land closest to sea
        // level, preferring columns nearer the centre on ties.
        //
        // Every column of that grid is decided by the density field alone, so the columns are
        // independent and the walk runs on as many threads as the machine offers. The order
        // the scan would have met a column is carried along and used as the last tie-breaker,
        // which keeps the answer the same one a single thread would have produced.
        SpawnPoint spawn_point(std::int32_t center_x = 8, std::int32_t center_z = 8,
            int radius = 256, int step = 16) const {
            SpawnPoint best;
            best.x = center_x;
            best.z = center_z;
            best.surface_y = surface_height(best.x, best.z);
            if (type_ == WorldType::Flat) {
                return best;
            }

            const int sea = sea_level();
            // Dry land first: a column under water puts the player in the sea. After that,
            // how close the surface is to sea level decides, the walk from the centre only
            // breaks ties - otherwise every far candidate loses to a peak next door - and
            // the scan order decides a tie the two others leave.
            struct Candidate {
                bool empty = true;
                bool land = false;
                int level = 0;
                int travel = 0;
                int order = 0;
                SpawnPoint point;
            };
            const auto beats = [](const Candidate& left, const Candidate& right) {
                if (right.empty) {
                    return !left.empty;
                }
                if (left.empty) {
                    return false;
                }
                if (left.land != right.land) {
                    return left.land;
                }
                if (left.level != right.level) {
                    return left.level < right.level;
                }
                if (left.travel != right.travel) {
                    return left.travel < right.travel;
                }
                return left.order < right.order;
            };

            Candidate chosen;
            chosen.empty = false;
            chosen.land = best.surface_y >= sea;
            chosen.level = std::abs(best.surface_y - sea);
            chosen.order = -1;
            chosen.point = best;

            const int steps = 2 * radius / step + 1;
            const unsigned hardware = std::thread::hardware_concurrency();
            const unsigned workers = std::max(1u, std::min<unsigned>(
                hardware > 1 ? hardware - 1 : 1u, static_cast<unsigned>(steps)));

            // One worker takes every `workers`-th row of the grid, so a row of expensive
            // mountains cannot land on a single thread.
            const auto scan = [&](unsigned worker) {
                Candidate local;
                for (int xi = static_cast<int>(worker); xi < steps; xi += static_cast<int>(workers)) {
                    const int dx = -radius + xi * step;
                    for (int zi = 0; zi < steps; ++zi) {
                        const int dz = -radius + zi * step;
                        const int y = surface_height(center_x + dx, center_z + dz);
                        Candidate candidate;
                        candidate.empty = false;
                        candidate.land = y >= sea;
                        candidate.level = std::abs(y - sea);
                        candidate.travel = std::abs(dx) + std::abs(dz);
                        candidate.order = xi * steps + zi;
                        candidate.point.x = center_x + dx;
                        candidate.point.z = center_z + dz;
                        candidate.point.surface_y = y;
                        if (beats(candidate, local)) {
                            local = candidate;
                        }
                    }
                }
                return local;
            };

            if (workers <= 1) {
                const Candidate single = scan(0);
                if (beats(single, chosen)) {
                    chosen = single;
                }
                return chosen.point;
            }

            std::vector<Candidate> results(workers);
            std::vector<std::thread> pool;
            pool.reserve(workers - 1);
            for (unsigned worker = 1; worker < workers; ++worker) {
                pool.emplace_back([&results, &scan, worker] {
                    results[worker] = scan(worker);
                });
            }
            results[0] = scan(0);
            for (auto& thread : pool) {
                thread.join();
            }

            for (const Candidate& candidate : results) {
                if (beats(candidate, chosen)) {
                    chosen = candidate;
                }
            }
            return chosen.point;
        }

        // The y of the block that keeps the sky out of a column: above it the column sees
        // the sky, which is what the natural spawner's light test reads.
        int sky_top(std::int32_t x, std::int32_t z) const {
            if (type_ == WorldType::Flat) {
                return min_y_ + spec_.dirt_layers + 1;
            }
            return terrain_.sky_top(x, z);
        }

        // A block in the four kinds the spawn rules test; a flat world is all air above its
        // grass layer.
        terrain::BlockKind block_kind_at(std::int32_t x, std::int32_t y,
            std::int32_t z) const {
            if (type_ != WorldType::Flat) {
                return terrain_.kind_at(x, y, z);
            }
            const std::int32_t state = state_at(x, y, z);
            if (state == air_) return terrain::BlockKind::Air;
            if (state == grass_) return terrain::BlockKind::Soil;
            if (state == bedrock_ || state == dirt_) return terrain::BlockKind::Solid;
            return terrain::BlockKind::Air;
        }

        std::int32_t state_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            if (type_ != WorldType::Flat) {
                return terrain_.state_at(x, y, z);
            }
            if (y < min_y_ || y >= min_y_ + section_count_ * 16) {
                return air_;
            }
            if (y == min_y_) {
                return bedrock_;
            }
            if (y < min_y_ + spec_.dirt_layers + 1) {
                return dirt_;
            }
            return y == min_y_ + spec_.dirt_layers + 1 ? grass_ : air_;
        }

        Chunk generate(std::int32_t cx, std::int32_t cz) const {
            if (type_ != WorldType::Flat) {
                return terrain_.generate(cx, cz);
            }
            if (auto chunk = flat::generate(cx, cz, spec_, block_, biome_)) {
                return std::move(*chunk);
            }
            Chunk empty;
            empty.x = cx;
            empty.z = cz;
            empty.min_y = min_y_;
            empty.section_count = section_count_;
            return empty;
        }

    private:
        static constexpr int kSeaLevel = 63;

        WorldType type_ = WorldType::Terrain;
        worldgen::DimensionKind kind_ = worldgen::DimensionKind::Overworld;
        terrain::Generator terrain_;
        flat::FlatSpec spec_;
        LightRules flat_rules_;
        terrain::IdResolver block_;
        terrain::IdResolver biome_;
        std::int32_t air_ = 0;
        std::int32_t bedrock_ = 0;
        std::int32_t dirt_ = 0;
        std::int32_t grass_ = 0;
        std::uint32_t seed_ = 0;
        int min_y_ = -64;
        int section_count_ = 24;
        bool ready_ = false;
    };

} // namespace terila::world
