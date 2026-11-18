#pragma once

// Trees and cacti. They are placed from a hash of the column, not from a feature queue, so a
// single block lookup, a chunk and the light pass all see the same plant. The column type is
// a template parameter because the generator's cached column and the plain one differ.

#include "world/terrain_biomes.hpp"
#include "world/terrain_noise.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace terila::world::worldgen {

    class Vegetation {
    public:
        static constexpr std::uint32_t kFeatureSeed = 0x7E41A5u;

        void init(const terrain::Blocks& blocks, const std::vector<terrain::Biome>& biomes,
            int sea_level) {
            blocks_ = blocks;
            biomes_ = biomes;
            sea_level_ = sea_level;
        }

        // The y of the block that keeps the sky out of a column: the surface, the water above
        // a flooded floor, or the highest leaf of a tree leaning into the column. Snow is
        // left out on purpose, it lets sunlight straight through.
        template<class ColumnFn>
        int sky_top(std::int32_t x, std::int32_t z, ColumnFn&& get_column) const {
            const auto col = get_column(x, z);
            int top = std::max(col.height, sea_level_ > col.height ? sea_level_ : col.height);

            for (std::int32_t tz = z - 2; tz <= z + 2; ++tz) {
                for (std::int32_t tx = x - 2; tx <= x + 2; ++tx) {
                    const auto plant = get_column(tx, tz);
                    const auto& biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }
                    const int dx = x - tx;
                    const int dz = z - tz;
                    const int distance = std::abs(dx) + std::abs(dz);
                    const bool trunk_column = dx == 0 && dz == 0;

                    if (has_tree(biome, tx, tz)) {
                        const int trunk = trunk_height(tx, tz);
                        if (trunk_column) {
                            top = std::max(top, plant.height + trunk);
                        }
                        for (int dy = trunk + 1; dy >= trunk - 2; --dy) {
                            const bool inside_trunk = trunk_column && dy <= trunk;
                            if (distance <= 3 && !inside_trunk &&
                                !(dy == trunk + 1 && distance > 1)) {
                                top = std::max(top, plant.height + dy);
                                break;
                            }
                        }
                    }
                    if (has_cactus(biome, tx, tz) && trunk_column) {
                        top = std::max(top, plant.height + cactus_height(tx, tz));
                    }
                }
            }
            return top;
        }

        // Replaces an air block with a plant block when one reaches it.
        template<class ColumnFn>
        std::int32_t apply(std::int32_t x, std::int32_t y, std::int32_t z,
            std::int32_t state, ColumnFn&& get_column) const {
            // Plants only ever stand on land, and only reach a few blocks above it, so every
            // flood of underground and underwater lookups the light engine makes skips the
            // whole neighbourhood scan.
            if (y <= sea_level_ || y > sea_level_ + 48) {
                return state;
            }
            for (std::int32_t tz = z - 2; tz <= z + 2; ++tz) {
                for (std::int32_t tx = x - 2; tx <= x + 2; ++tx) {
                    const auto plant = get_column(tx, tz);
                    const auto& plant_biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }
                    if (has_tree(plant_biome, tx, tz)) {
                        const int trunk = trunk_height(tx, tz);
                        if (tx == x && tz == z && y > plant.height &&
                            y <= plant.height + trunk) {
                            state = plant_biome.log;
                        }
                        const int dx = x - tx;
                        const int dz = z - tz;
                        const int dy = y - plant.height;
                        const int distance = std::abs(dx) + std::abs(dz);
                        const bool inside_trunk = dx == 0 && dz == 0 && dy <= trunk;
                        if (dy >= trunk - 2 && dy <= trunk + 1 && distance <= 3 &&
                            !inside_trunk && !(dy == trunk + 1 && distance > 1)) {
                            state = plant_biome.leaves;
                        }
                    }
                    if (has_cactus(plant_biome, tx, tz) && tx == x && tz == z &&
                        y > plant.height && y <= plant.height + cactus_height(tx, tz)) {
                        state = blocks_.cactus;
                    }
                }
            }
            return state;
        }

        int trunk_height(std::int32_t x, std::int32_t z) const {
            return 4 + static_cast<int>(terrain::detail::hash3(x, z, kFeatureSeed + 1) % 3);
        }

        int cactus_height(std::int32_t x, std::int32_t z) const {
            return 1 + static_cast<int>(terrain::detail::hash3(x, z, kFeatureSeed + 3) % 3);
        }

        bool has_tree(const terrain::Biome& biome, std::int32_t x, std::int32_t z) const {
            return biome.tree_chance > 0 &&
                terrain::detail::hash3(x, z, kFeatureSeed) %
                static_cast<std::uint32_t>(biome.tree_chance) == 0;
        }

        bool has_cactus(const terrain::Biome& biome, std::int32_t x, std::int32_t z) const {
            return biome.cactus_chance > 0 &&
                terrain::detail::hash3(x, z, kFeatureSeed + 2) %
                static_cast<std::uint32_t>(biome.cactus_chance) == 0;
        }

    private:
        terrain::Blocks blocks_{};
        std::vector<terrain::Biome> biomes_;
        int sea_level_ = 63;
    };

} // namespace terila::world::worldgen
