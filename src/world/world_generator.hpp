#pragma once

#include "log/logger.hpp"
#include "world/chunk.hpp"
#include "world/flat_generator.hpp"
#include "world/terrain_generator.hpp"

#include <cstdint>
#include <functional>
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
            const terrain::IdResolver& block, const terrain::IdResolver& biome) {
            type_ = type;
            min_y_ = min_y;
            section_count_ = section_count;
            seed_ = seed;
            block_ = block;
            biome_ = biome;

            if (type_ != WorldType::Flat) {
                ready_ = terrain_.init(min_y, section_count, seed, block, biome);
                return ready_;
            }

            spec_.min_y = min_y;
            spec_.section_count = section_count;

            air_ = block(spec_.air).value_or(0);
            bedrock_ = block(spec_.bedrock).value_or(0);
            dirt_ = block(spec_.dirt).value_or(0);
            grass_ = block(spec_.grass).value_or(0);

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
        WorldType type() const { return type_; }
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
        terrain::Generator terrain_;
        flat::FlatSpec spec_;
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
