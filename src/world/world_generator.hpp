#pragma once

#include "log/logger.hpp"
#include "world/chunk.hpp"
#include "world/flat_generator.hpp"
#include "world/terrain_generator.hpp"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
            bool best_land = best.surface_y >= sea;
            int best_level = std::abs(best.surface_y - sea);
            int best_travel = 0;

            for (int dx = -radius; dx <= radius; dx += step) {
                for (int dz = -radius; dz <= radius; dz += step) {
                    const int y = surface_height(center_x + dx, center_z + dz);
                    // Dry land first: a column under water puts the player in the sea.
                    // After that, how close the surface is to sea level decides, and the
                    // walk from the centre only breaks ties - otherwise every candidate
                    // far from the centre loses to a peak right next to it.
                    const bool land = y >= sea;
                    const int level = std::abs(y - sea);
                    const int travel = std::abs(dx) + std::abs(dz);

                    bool better = false;
                    if (land != best_land) {
                        better = land;
                    }
                    else if (level != best_level) {
                        better = level < best_level;
                    }
                    else {
                        better = travel < best_travel;
                    }

                    if (better) {
                        best_land = land;
                        best_level = level;
                        best_travel = travel;
                        best.x = center_x + dx;
                        best.z = center_z + dz;
                        best.surface_y = y;
                    }
                }
            }
            return best;
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
