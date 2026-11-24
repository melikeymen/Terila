#pragma once

// Trees and cacti. They are placed from a hash of the column, not from a feature queue, so a
// single block lookup, a chunk and the light pass all see the same plant. The column type is
// a template parameter because the generator's cached column and the plain one differ.

#include "world/terrain_biomes.hpp"
#include "world/terrain_noise.hpp"
#include "world/worldgen/feature/state_provider.hpp"

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
            int sea_level, const std::vector<StateProvider>& providers = {}) {
            blocks_ = blocks;
            biomes_ = biomes;
            sea_level_ = sea_level;
            providers_ = providers;
        }

        // The y of the block that keeps the sky out of a column: the surface, the water above
        // a flooded floor, or the highest leaf of a tree leaning into the column. Snow is
        // left out on purpose, it lets sunlight straight through.
        template<class ColumnFn>
        int sky_top(std::int32_t x, std::int32_t z, ColumnFn&& get_column) const {
            int top = base_top(get_column(x, z));

            for (std::int32_t tz = z - 2; tz <= z + 2; ++tz) {
                for (std::int32_t tx = x - 2; tx <= x + 2; ++tx) {
                    const auto plant = get_column(tx, tz);
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }
                    const auto& biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    const bool tree = has_tree(biome, tx, tz);
                    const bool cactus = has_cactus(biome, tx, tz);
                    if (!tree && !cactus) {
                        continue;
                    }
                    top = std::max(top,
                        lift(plant.height, x - tx, z - tz, tree,
                            tree ? trunk_height(biome, tx, tz) : 0,
                            cactus ? cactus_height(tx, tz) : 0));
                }
            }
            return top;
        }

        // The same answer for every column of a square, built the other way round: each plant
        // lifts the columns it reaches instead of each column scanning the plants around it.
        // The light pass asks for a fifteen block halo, 2116 columns per chunk, and scanning
        // them one by one costs as much as the rest of the chunk together.
        template<class ColumnFn>
        void sky_tops(std::int32_t x0, std::int32_t z0, int side, ColumnFn&& get_column,
            std::vector<int>& out) const {
            out.resize(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
            for (int z = 0; z < side; ++z) {
                for (int x = 0; x < side; ++x) {
                    out[static_cast<std::size_t>(z) * static_cast<std::size_t>(side) +
                        static_cast<std::size_t>(x)] = base_top(get_column(x0 + x, z0 + z));
                }
            }

            for (std::int32_t pz = z0 - 2; pz < z0 + side + 2; ++pz) {
                for (std::int32_t px = x0 - 2; px < x0 + side + 2; ++px) {
                    const auto plant = get_column(px, pz);
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }
                    const auto& biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    const bool tree = has_tree(biome, px, pz);
                    const bool cactus = has_cactus(biome, px, pz);
                    if (!tree && !cactus) {
                        continue;
                    }
                    const int trunk = tree ? trunk_height(biome, px, pz) : 0;
                    const int cactus_height = cactus ? this->cactus_height(px, pz) : 0;
                    for (int dz = -2; dz <= 2; ++dz) {
                        const int tz = pz + dz - z0;
                        if (tz < 0 || tz >= side) {
                            continue;
                        }
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int tx = px + dx - x0;
                            if (tx < 0 || tx >= side) {
                                continue;
                            }
                            int& cell = out[static_cast<std::size_t>(tz) *
                                static_cast<std::size_t>(side) + static_cast<std::size_t>(tx)];
                            cell = std::max(cell,
                                lift(plant.height, dx, dz, tree, trunk, cactus_height));
                        }
                    }
                }
            }
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
                        const auto species = species_of(plant_biome, tx, tz);
                        const int trunk = trunk_height(plant_biome, tx, tz);
                        if (tx == x && tz == z && y > plant.height &&
                            y <= plant.height + trunk) {
                            state = species.log;
                        }
                        const int dx = x - tx;
                        const int dz = z - tz;
                        const int dy = y - plant.height;
                        const int distance = std::abs(dx) + std::abs(dz);
                        const bool inside_trunk = dx == 0 && dz == 0 && dy <= trunk;
                        if (dy >= trunk - 2 && dy <= trunk + 1 && distance <= 3 &&
                            !inside_trunk && !(dy == trunk + 1 && distance > 1)) {
                            state = species.leaves;
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

        // The species of the tree that owns a column. Vanilla picks it from the selector the
        // biome names; a hash of the column stands in for that roll, so every reader of the
        // column picks the same species.
        terrain::TreeSpecies species_of(const terrain::Biome& biome, std::int32_t x,
            std::int32_t z) const {
            if (biome.species.empty()) {
                terrain::TreeSpecies plain;
                plain.log = biome.log;
                plain.leaves = biome.leaves;
                return plain;
            }
            if (biome.species.size() == 1) {
                return biome.species.front();
            }
            std::uint32_t total = 0;
            for (const auto& species : biome.species) {
                total += static_cast<std::uint32_t>(std::max(1, species.weight));
            }
            std::uint32_t pick = terrain::detail::hash3(x, z, kFeatureSeed + 13) % total;
            for (const auto& species : biome.species) {
                const auto weight = static_cast<std::uint32_t>(std::max(1, species.weight));
                if (pick < weight) {
                    return species;
                }
                pick -= weight;
            }
            return biome.species.back();
        }

        int trunk_height(const terrain::Biome& biome, std::int32_t x, std::int32_t z) const {
            const terrain::TreeSpecies species = species_of(biome, x, z);
            const int span = std::max(0, species.max_trunk - species.min_trunk);
            return species.min_trunk + static_cast<int>(
                terrain::detail::hash3(x, z, kFeatureSeed + 1) %
                static_cast<std::uint32_t>(span + 1));
        }

        // The part of a sky top that no plant is responsible for: the surface, or the sea
        // above a flooded floor.
        template<class Column>
        int base_top(const Column& column) const {
            return std::max(column.height,
                sea_level_ > column.height ? sea_level_ : column.height);
        }

        // How far one plant lifts a neighbouring column, or nothing at all. The shape is the
        // leaf blob: the trunk column keeps the sky out to the trunk top, a directly adjacent
        // column up to one block above it, and every column within three blocks to the trunk
        // top. Answering nothing matters, because a hillside neighbour must not raise a lower
        // column to its own surface.
        static constexpr int kNoLift = -1000000;

        int lift(int height, int dx, int dz, bool tree, int trunk, int cactus) const {
            const int distance = std::abs(dx) + std::abs(dz);
            int top = kNoLift;
            if (tree) {
                if (distance == 0) {
                    top = height + trunk;
                }
                else if (distance == 1) {
                    top = height + trunk + 1;
                }
                else if (distance <= 3) {
                    top = height + trunk;
                }
            }
            if (cactus > 0 && distance == 0) {
                top = std::max(top, height + cactus);
            }
            return top;
        }

        int cactus_height(std::int32_t x, std::int32_t z) const {
            return 1 + static_cast<int>(terrain::detail::hash3(x, z, kFeatureSeed + 3) % 3);
        }

        // The ground plant that stands on an undisturbed surface column, or 0 for none. A
        // flower takes the grass's place where the flower hash fires, and the two hashes are
        // independent so a meadow mixes both. When the data names a provider, the provider
        // picks the species itself, from the field it carries at this block.
        std::int32_t cover(const terrain::Biome& biome, std::int32_t x, std::int32_t ground,
            std::int32_t z) const {
            const std::int32_t y = ground + 1;
            if (biome.flower_chance > 0 &&
                terrain::detail::hash3(x, z, kFeatureSeed + 11) %
                    static_cast<std::uint32_t>(biome.flower_chance) == 0) {
                const std::int32_t state = provider_state(biome.flower_provider, x, y, z);
                if (state != 0) {
                    return state;
                }
                if (biome.flower != 0) {
                    return biome.flower;
                }
            }
            if (biome.cover_chance > 0 &&
                terrain::detail::hash3(x, z, kFeatureSeed + 7) %
                    static_cast<std::uint32_t>(biome.cover_chance) == 0) {
                const std::int32_t state = provider_state(biome.cover_provider, x, y, z);
                return state != 0 ? state : biome.cover;
            }
            return 0;
        }

        std::int32_t provider_state(int index, std::int32_t x, std::int32_t y,
            std::int32_t z) const {
            if (index < 0 || static_cast<std::size_t>(index) >= providers_.size()) {
                return 0;
            }
            return providers_[static_cast<std::size_t>(index)].state_at(x, y, z,
                static_cast<std::uint32_t>(index) * 0x9E3779B9u);
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
        std::vector<StateProvider> providers_;
        int sea_level_ = 63;
    };

} // namespace terila::world::worldgen
