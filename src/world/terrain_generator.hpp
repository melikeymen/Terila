#pragma once

#include "world/chunk.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace terila::world::terrain {

    using IdResolver = std::function<std::optional<std::int32_t>(std::string_view)>;

    namespace detail {

        inline std::uint32_t hash3(std::int32_t x, std::int32_t z, std::uint32_t seed) {
            std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::int64_t>(x)) *
                0x9E3779B97F4A7C15ull;
            h ^= static_cast<std::uint64_t>(static_cast<std::int64_t>(z)) * 0xC2B2AE3D27D4EB4Full;
            h ^= static_cast<std::uint64_t>(seed) * 0x165667B19E3779F9ull;
            h ^= h >> 33;
            h *= 0xFF51AFD7ED558CCDull;
            h ^= h >> 33;
            h *= 0xC4CEB9FE1A85EC53ull;
            h ^= h >> 33;
            return static_cast<std::uint32_t>(h >> 32);
        }

        // Value noise on a unit lattice, bilinearly blended with a smoothstep weight.
        inline float noise(float x, float z, std::uint32_t seed) {
            const auto xi = static_cast<std::int32_t>(std::floor(x));
            const auto zi = static_cast<std::int32_t>(std::floor(z));
            const float fx = x - static_cast<float>(xi);
            const float fz = z - static_cast<float>(zi);
            const float u = fx * fx * (3.0f - 2.0f * fx);
            const float v = fz * fz * (3.0f - 2.0f * fz);

            const auto sample = [&](std::int32_t ox, std::int32_t oz) {
                return (hash3(xi + ox, zi + oz, seed) & 0xFFFFFF) / 8388607.5f - 1.0f;
            };

            const float a = sample(0, 0);
            const float b = sample(1, 0);
            const float c = sample(0, 1);
            const float d = sample(1, 1);
            const float top = a + (b - a) * u;
            const float bottom = c + (d - c) * u;
            return top + (bottom - top) * v;
        }

        inline float fbm(float x, float z, std::uint32_t seed, int octaves) {
            float total = 0.0f;
            float amplitude = 1.0f;
            float frequency = 1.0f;
            float norm = 0.0f;

            for (int octave = 0; octave < octaves; ++octave) {
                total += noise(x * frequency, z * frequency, seed + octave * 0x9E37u) * amplitude;
                norm += amplitude;
                amplitude *= 0.5f;
                frequency *= 2.0f;
            }
            return norm > 0.0f ? total / norm : 0.0f;
        }

    } // namespace detail

    struct Blocks {
        std::int32_t air = 0;
        std::int32_t bedrock = 0;
        std::int32_t stone = 0;
        std::int32_t dirt = 0;
        std::int32_t grass = 0;
        std::int32_t sand = 0;
        std::int32_t sandstone = 0;
        std::int32_t gravel = 0;
        std::int32_t water = 0;
        std::int32_t snow = 0;
        std::int32_t oak_log = 0;
        std::int32_t oak_leaves = 0;
        std::int32_t spruce_log = 0;
        std::int32_t spruce_leaves = 0;
        std::int32_t acacia_log = 0;
        std::int32_t acacia_leaves = 0;
        std::int32_t cactus = 0;
    };

    // One entry of the biome table: what the surface looks like and what grows on it.
    struct Biome {
        std::int32_t id = 0;          // biome registry id
        std::int32_t surface = 0;     // top block
        std::int32_t filler = 0;      // three blocks under the surface
        std::int32_t deep = 0;        // everything below the filler
        std::int32_t log = 0;
        std::int32_t leaves = 0;
        int tree_chance = 0;          // one tree per N columns, 0 disables trees
        int cactus_chance = 0;        // one cactus per N columns
        bool snow_cap = false;        // a snow layer on top of the surface
        int snow_above = 0;           // snow above this surface height, 0 disables
    };

    // Deterministic heightmap + biome generator. Chunk generation and single block
    // lookups share the same column function, so an untouched block always equals the
    // generated one and the overlay only stores real edits.
    class Generator {
    public:
        bool init(int min_y, int section_count, std::uint32_t seed,
            const IdResolver& block, const IdResolver& biome) {
            min_y_ = min_y;
            section_count_ = section_count;
            seed_ = seed;

            bool complete = true;
            const auto id = [&](const char* name) {
                const auto resolved = block(name);
                if (!resolved) {
                    LOG_WARN(Chunk, "terrain: cannot resolve block '{}'", name);
                    complete = false;
                    return 0;
                }
                return *resolved;
            };

            blocks_.air = id("minecraft:air");
            blocks_.bedrock = id("minecraft:bedrock");
            blocks_.stone = id("minecraft:stone");
            blocks_.dirt = id("minecraft:dirt");
            blocks_.grass = id("minecraft:grass_block");
            blocks_.sand = id("minecraft:sand");
            blocks_.sandstone = id("minecraft:sandstone");
            blocks_.gravel = id("minecraft:gravel");
            blocks_.water = id("minecraft:water");
            blocks_.snow = id("minecraft:snow");
            blocks_.oak_log = id("minecraft:oak_log");
            blocks_.oak_leaves = id("minecraft:oak_leaves");
            blocks_.spruce_log = id("minecraft:spruce_log");
            blocks_.spruce_leaves = id("minecraft:spruce_leaves");
            blocks_.acacia_log = id("minecraft:acacia_log");
            blocks_.acacia_leaves = id("minecraft:acacia_leaves");
            blocks_.cactus = id("minecraft:cactus");

            const auto add = [&](const char* name, std::int32_t surface, std::int32_t filler,
                std::int32_t deep, std::int32_t log, std::int32_t leaves, int trees,
                int cacti, bool snow, int snow_above = 0) {
                const auto resolved = biome(name);
                if (!resolved) {
                    LOG_WARN(Chunk, "terrain: cannot resolve biome '{}'", name);
                    biomes_.push_back(Biome{});
                    complete = false;
                    return;
                }
                biomes_.push_back(Biome{ *resolved, surface, filler, deep, log, leaves,
                    trees, cacti, snow, snow_above });
            };

            add("minecraft:ocean", blocks_.gravel, blocks_.gravel, blocks_.stone,
                0, 0, 0, 0, false);
            add("minecraft:beach", blocks_.sand, blocks_.sand, blocks_.sandstone,
                0, 0, 0, 0, false);
            add("minecraft:plains", blocks_.grass, blocks_.dirt, blocks_.stone,
                blocks_.oak_log, blocks_.oak_leaves, 90, 0, false);
            add("minecraft:forest", blocks_.grass, blocks_.dirt, blocks_.stone,
                blocks_.oak_log, blocks_.oak_leaves, 20, 0, false);
            add("minecraft:desert", blocks_.sand, blocks_.sand, blocks_.sandstone,
                0, 0, 0, 55, false);
            add("minecraft:snowy_plains", blocks_.grass, blocks_.dirt, blocks_.stone,
                blocks_.spruce_log, blocks_.spruce_leaves, 160, 0, true);
            add("minecraft:taiga", blocks_.grass, blocks_.dirt, blocks_.stone,
                blocks_.spruce_log, blocks_.spruce_leaves, 28, 0, false);
            add("minecraft:savanna", blocks_.grass, blocks_.dirt, blocks_.stone,
                blocks_.acacia_log, blocks_.acacia_leaves, 60, 0, false);
            add("minecraft:stony_peaks", blocks_.stone, blocks_.stone, blocks_.stone,
                0, 0, 0, 0, false, 116);
            add("minecraft:deep_ocean", blocks_.gravel, blocks_.stone, blocks_.stone,
                0, 0, 0, 0, false);

            ready_ = complete && biomes_.size() == kBiomeCount;
            return ready_;
        }

        bool ready() const { return ready_; }
        std::uint32_t seed() const { return seed_; }
        int min_y() const { return min_y_; }
        int section_count() const { return section_count_; }
        int sea_level() const { return sea_level_; }

        // Highest solid block of a column, water and plants excluded.
        int surface_height(std::int32_t x, std::int32_t z) const {
            return column(x, z).height;
        }

        std::int32_t state_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            const Column col = column(x, z);
            const Biome& biome = biomes_[static_cast<std::size_t>(col.biome)];

            if (y < min_y_ || y >= min_y_ + section_count_ * 16) {
                return blocks_.air;
            }

            std::int32_t state = blocks_.air;
            if (y == min_y_) {
                state = blocks_.bedrock;
            }
            else if (y <= col.height) {
                if (y == col.height) {
                    state = biome.surface;
                }
                else if (y >= col.height - 3) {
                    state = biome.filler;
                }
                else {
                    state = biome.deep;
                }
            }
            else if (y <= sea_level_) {
                // Water is written after the snow layer, so it wins on flooded columns.
                state = blocks_.water;
            }
            else if (y == col.height + 1 &&
                (biome.snow_cap || (biome.snow_above > 0 && col.height >= biome.snow_above))) {
                state = blocks_.snow;
            }

            // Plants belong to their own column, so a cell can be claimed by a tree
            // whose trunk stands up to two blocks away. Generation writes plants in this
            // same order over the terrain, so the last match is the visible block.
            for (std::int32_t tz = z - 2; tz <= z + 2; ++tz) {
                for (std::int32_t tx = x - 2; tx <= x + 2; ++tx) {
                    const Column plant = column(tx, tz);
                    const Biome& plant_biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }
                    if (plant_biome.tree_chance > 0 &&
                        detail::hash3(tx, tz, kFeatureSeed) % static_cast<std::uint32_t>(
                            plant_biome.tree_chance) == 0) {
                        const int trunk = 4 + static_cast<int>(
                            detail::hash3(tx, tz, kFeatureSeed + 1) % 3);
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
                    if (plant_biome.cactus_chance > 0 &&
                        detail::hash3(tx, tz, kFeatureSeed + 2) % static_cast<std::uint32_t>(
                            plant_biome.cactus_chance) == 0) {
                        const int height = 1 + static_cast<int>(
                            detail::hash3(tx, tz, kFeatureSeed + 3) % 3);
                        if (tx == x && tz == z && y > plant.height &&
                            y <= plant.height + height) {
                            state = blocks_.cactus;
                        }
                    }
                }
            }

            return state;
        }

        Chunk generate(std::int32_t cx, std::int32_t cz) const {
            Chunk chunk;
            chunk.x = cx;
            chunk.z = cz;
            chunk.min_y = min_y_;
            chunk.section_count = section_count_;

            const int world_height = section_count_ * 16;
            std::vector<std::int32_t> blocks(
                static_cast<std::size_t>(world_height) * 256, blocks_.air);

            const std::int32_t wx0 = cx * 16;
            const std::int32_t wz0 = cz * 16;

            const auto put = [&](std::int32_t wx, std::int32_t y, std::int32_t wz,
                std::int32_t state) {
                if (y < min_y_ || y >= min_y_ + world_height) {
                    return;
                }
                const std::int32_t lx = wx - wx0;
                const std::int32_t lz = wz - wz0;
                if (lx < 0 || lx > 15 || lz < 0 || lz > 15) {
                    return;
                }
                blocks[static_cast<std::size_t>(y - min_y_) * 256 +
                    static_cast<std::size_t>(lz) * 16 + static_cast<std::size_t>(lx)] = state;
            };

            std::array<int, 256> column_heights{};
            std::array<int, 256> column_biomes{};

            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    const Column col = column(wx0 + lx, wz0 + lz);
                    const Biome& biome = biomes_[static_cast<std::size_t>(col.biome)];
                    column_heights[static_cast<std::size_t>(lz * 16 + lx)] = col.height;
                    column_biomes[static_cast<std::size_t>(lz * 16 + lx)] = col.biome;

                    put(wx0 + lx, min_y_, wz0 + lz, blocks_.bedrock);
                    for (int y = min_y_ + 1; y <= col.height; ++y) {
                        std::int32_t state = biome.deep;
                        if (y == col.height) {
                            state = biome.surface;
                        }
                        else if (y >= col.height - 3) {
                            state = biome.filler;
                        }
                        put(wx0 + lx, y, wz0 + lz, state);
                    }
                    if (biome.snow_cap ||
                        (biome.snow_above > 0 && col.height >= biome.snow_above)) {
                        put(wx0 + lx, col.height + 1, wz0 + lz, blocks_.snow);
                    }
                    for (int y = col.height + 1; y <= sea_level_ && y > min_y_; ++y) {
                        put(wx0 + lx, y, wz0 + lz, blocks_.water);
                    }
                }
            }

            // Trees and cacti: the margin lets plants from neighbouring columns lean in.
            for (std::int32_t tz = wz0 - 2; tz <= wz0 + 17; ++tz) {
                for (std::int32_t tx = wx0 - 2; tx <= wx0 + 17; ++tx) {
                    const Column plant = column(tx, tz);
                    const Biome& biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }

                    if (biome.tree_chance > 0 &&
                        detail::hash3(tx, tz, kFeatureSeed) % static_cast<std::uint32_t>(
                            biome.tree_chance) == 0) {
                        const int trunk = 4 + static_cast<int>(
                            detail::hash3(tx, tz, kFeatureSeed + 1) % 3);
                        for (int i = 1; i <= trunk; ++i) {
                            put(tx, plant.height + i, tz, biome.log);
                        }

                        for (int dy = trunk - 2; dy <= trunk + 1; ++dy) {
                            for (int dz = -2; dz <= 2; ++dz) {
                                for (int dx = -2; dx <= 2; ++dx) {
                                    const int distance = std::abs(dx) + std::abs(dz);
                                    if (distance > 3) continue;
                                    if (dx == 0 && dz == 0 && dy <= trunk) continue;
                                    if (dy == trunk + 1 && distance > 1) continue;
                                    put(tx + dx, plant.height + dy, tz + dz, biome.leaves);
                                }
                            }
                        }
                        continue;
                    }

                    if (biome.cactus_chance > 0 &&
                        detail::hash3(tx, tz, kFeatureSeed + 2) % static_cast<std::uint32_t>(
                            biome.cactus_chance) == 0) {
                        const int height = 1 + static_cast<int>(
                            detail::hash3(tx, tz, kFeatureSeed + 3) % 3);
                        for (int i = 1; i <= height; ++i) {
                            put(tx, plant.height + i, tz, blocks_.cactus);
                        }
                    }
                }
            }

            // Heightmaps: nine bits per column of "first air above the surface".
            std::array<int, 256> top_blocks{};
            top_blocks.fill(min_y_);
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    for (int y = min_y_ + world_height - 1; y >= min_y_; --y) {
                        const auto state = blocks[static_cast<std::size_t>(y - min_y_) * 256 +
                            static_cast<std::size_t>(lz) * 16 + static_cast<std::size_t>(lx)];
                        if (state != blocks_.air) {
                            top_blocks[static_cast<std::size_t>(lz * 16 + lx)] = y + 1;
                            break;
                        }
                    }
                }
            }

            std::vector<std::int64_t> height_longs(36, 0);
            for (int column_index = 0; column_index < 256; ++column_index) {
                const std::uint64_t value = static_cast<std::uint64_t>(
                    top_blocks[static_cast<std::size_t>(column_index)] - min_y_) & 0x1FF;
                const int bit = column_index * 9;
                const int long_index = bit / 64;
                const int offset = bit % 64;
                height_longs[static_cast<std::size_t>(long_index)] |=
                    static_cast<std::int64_t>(value << offset);
                if (offset > 55) {
                    height_longs[static_cast<std::size_t>(long_index) + 1] |=
                        static_cast<std::int64_t>(value >> (64 - offset));
                }
            }
            for (int type : {1, 4, 5}) {
                Heightmap heightmap;
                heightmap.type_id = type;
                heightmap.longs = height_longs;
                chunk.heightmaps.push_back(std::move(heightmap));
            }

            // Sections: palettes are built from the blocks actually present per section.
            chunk.sections.resize(static_cast<std::size_t>(section_count_));
            for (int sy = 0; sy < section_count_; ++sy) {
                ChunkSection& section = chunk.sections[static_cast<std::size_t>(sy)];
                std::unordered_map<std::int32_t, std::int32_t> lookup;
                std::vector<std::int32_t> palette;
                std::vector<std::int32_t> indices(4096, 0);
                int non_air = 0;
                int fluids = 0;

                for (int i = 0; i < 4096; ++i) {
                    const std::int32_t state = blocks[static_cast<std::size_t>(sy) * 4096 +
                        static_cast<std::size_t>(i)];
                    auto entry = lookup.find(state);
                    if (entry == lookup.end()) {
                        const auto index = static_cast<std::int32_t>(palette.size());
                        palette.push_back(state);
                        entry = lookup.emplace(state, index).first;
                    }
                    indices[static_cast<std::size_t>(i)] = entry->second;
                    if (state != blocks_.air) ++non_air;
                    if (state == blocks_.water) ++fluids;
                }

                section.block_count = static_cast<std::uint16_t>(non_air);
                section.fluid_count = static_cast<std::uint16_t>(fluids);
                if (palette.size() == 1) {
                    section.block_bits = 0;
                    section.block_palette = { palette[0] };
                }
                else {
                    int bits = 4;
                    while (bits < 8 && (1u << bits) < palette.size()) {
                        ++bits;
                    }
                    section.block_bits = static_cast<std::uint8_t>(bits);
                    section.block_palette = palette;
                    section.block_states = indices;
                }

                // Biomes are 4x4x4 cells; the column biome at the cell centre is used.
                std::unordered_map<std::int32_t, std::int32_t> biome_lookup;
                std::vector<std::int32_t> biome_palette;
                std::vector<std::int32_t> biome_indices(64, 0);
                for (int cy = 0; cy < 4; ++cy) {
                    for (int cz = 0; cz < 4; ++cz) {
                        for (int cxx = 0; cxx < 4; ++cxx) {
                            const int lx = std::min(15, cxx * 4 + 2);
                            const int lz = std::min(15, cz * 4 + 2);
                            const std::int32_t biome_id = biomes_[static_cast<std::size_t>(
                                column_biomes[static_cast<std::size_t>(lz * 16 + lx)])].id;
                            auto entry = biome_lookup.find(biome_id);
                            if (entry == biome_lookup.end()) {
                                const auto index = static_cast<std::int32_t>(
                                    biome_palette.size());
                                biome_palette.push_back(biome_id);
                                entry = biome_lookup.emplace(biome_id, index).first;
                            }
                            biome_indices[static_cast<std::size_t>((cy * 4 + cz) * 4 + cxx)] =
                                entry->second;
                        }
                    }
                }
                if (biome_palette.size() == 1) {
                    section.biome_bits = 0;
                    section.biome_palette = { biome_palette[0] };
                }
                else {
                    int bits = 1;
                    while (bits < 4 && (1u << bits) < biome_palette.size()) {
                        ++bits;
                    }
                    section.biome_bits = static_cast<std::uint8_t>(bits);
                    section.biome_palette = biome_palette;
                    section.biome_states = biome_indices;
                }
            }

            // Sky light: full above the highest solid block of each column, dark below.
            std::array<int, 256> top_solid{};
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    int top = min_y_ - 1;
                    for (int y = min_y_ + world_height - 1; y >= min_y_; --y) {
                        const auto state = blocks[static_cast<std::size_t>(y - min_y_) * 256 +
                            static_cast<std::size_t>(lz) * 16 + static_cast<std::size_t>(lx)];
                        if (state != blocks_.air && state != blocks_.water) {
                            top = y;
                            break;
                        }
                    }
                    top_solid[static_cast<std::size_t>(lz * 16 + lx)] = top;
                }
            }

            int min_top = *std::min_element(top_solid.begin(), top_solid.end());
            int max_top = *std::max_element(top_solid.begin(), top_solid.end());

            std::vector<std::int64_t> sky_mask(1 + (static_cast<std::size_t>(section_count_) + 1) / 64, 0);
            std::vector<std::int64_t> empty_sky_mask = sky_mask;

            const auto set_bit = [](std::vector<std::int64_t>& mask, int bit) {
                mask[static_cast<std::size_t>(bit / 64)] |= (1LL << (bit % 64));
            };

            for (int sy = 0; sy < section_count_; ++sy) {
                const int section_bottom = min_y_ + sy * 16;
                const int section_top = section_bottom + 15;

                if (section_bottom > max_top) {
                    set_bit(empty_sky_mask, sy + 1);
                    continue;
                }
                if (section_top <= min_top) {
                    continue;
                }

                std::vector<std::uint8_t> sky(2048, 0);
                for (int y = 0; y < 16; ++y) {
                    for (int z = 0; z < 16; ++z) {
                        for (int x = 0; x < 16; ++x) {
                            const int world_y = section_bottom + y;
                            if (world_y <= top_solid[static_cast<std::size_t>(z * 16 + x)]) {
                                continue;
                            }
                            const int index = (y * 16 + z) * 16 + x;
                            const auto byte_index = static_cast<std::size_t>(index / 2);
                            if (index % 2 == 0) {
                                sky[byte_index] |= 0x0F;
                            }
                            else {
                                sky[byte_index] |= 0xF0;
                            }
                        }
                    }
                }
                set_bit(sky_mask, sy + 1);
                chunk.light.sky_updates.push_back(std::move(sky));
            }

            chunk.light.sky_y_mask = sky_mask;
            chunk.light.empty_sky_y_mask = empty_sky_mask;
            if (!chunk.light.sky_updates.empty()) {
                chunk.light.sky_y_mask.push_back(0);   // keep masks long-aligned only
            }

            // No block light anywhere.
            std::vector<std::int64_t> block_mask(sky_mask.size(), 0);
            std::vector<std::int64_t> empty_block_mask(sky_mask.size(), 0);
            for (int sy = 0; sy < section_count_; ++sy) {
                set_bit(empty_block_mask, sy + 1);
            }
            chunk.light.block_y_mask = block_mask;
            chunk.light.empty_block_y_mask = empty_block_mask;

            return chunk;
        }

    private:
        static constexpr std::size_t kBiomeCount = 10;
        static constexpr std::uint32_t kFeatureSeed = 0x7E41A5u;

        struct Column {
            int height = 0;
            int biome = 0;
        };

        Column column(std::int32_t x, std::int32_t z) const {
            const float fx = static_cast<float>(x);
            const float fz = static_cast<float>(z);

            // Vanilla-style shaping: a continentalness spline decides between deep
            // ocean, shore and inland, and low erosion turns inland ridges into peaks.
            const float continents = detail::fbm(fx / 1400.0f, fz / 1400.0f, seed_ + 11u, 4);
            const float erosion = detail::fbm(fx / 760.0f, fz / 760.0f, seed_ + 23u, 3);
            const float ridge = 1.0f - std::abs(detail::noise(fx / 220.0f, fz / 220.0f,
                seed_ + 31u));
            const float bumps = 1.0f - std::abs(detail::noise(fx / 90.0f, fz / 90.0f,
                seed_ + 37u));
            const float detail_noise = detail::noise(fx / 30.0f, fz / 30.0f, seed_ + 47u);

            const float cont = std::clamp(continents * 1.25f, -1.0f, 1.0f);
            float height_f = 0.0f;
            if (cont < -0.2f) {
                const float shore = (cont + 1.0f) / 0.8f;          // 0 deep .. 1 shore
                height_f = 34.0f + shore * 26.0f + detail_noise * 2.0f;
            }
            else {
                const float inland = std::min(1.0f, (cont + 0.2f) / 1.2f);
                const float base = 63.0f + inland * 22.0f;
                const float exposure = 1.0f -
                    std::min(0.85f, std::abs(erosion) + inland * 0.1f);
                const float mountain = std::max(0.0f, ridge - 0.45f) * exposure * 150.0f;
                height_f = base + mountain + bumps * 6.0f * inland + detail_noise * 2.5f;
            }

            const int max_y = min_y_ + section_count_ * 16 - 3;
            Column result;
            result.height = std::clamp(static_cast<int>(std::lround(height_f)),
                min_y_ + 3, max_y);

            const float temperature = detail::fbm(fx / 900.0f, fz / 900.0f, seed_ + 101u, 2);
            const float humidity = detail::fbm(fx / 640.0f, fz / 640.0f, seed_ + 211u, 2);

            int index = 2;   // plains
            if (result.height < sea_level_ - 28) {
                index = 9;   // deep ocean
            }
            else if (result.height < sea_level_ - 1) {
                index = 0;   // ocean
            }
            else if (result.height <= sea_level_ + 2) {
                index = 1;   // beach
            }
            else if (result.height > sea_level_ + 46) {
                index = 8;   // stony peaks
            }
            else if (temperature < -0.35f) {
                index = 5;   // snowy plains
            }
            else if (temperature < -0.12f) {
                index = 6;   // taiga
            }
            else if (humidity > 0.3f) {
                index = 3;   // forest
            }
            else if (temperature > 0.35f && humidity < 0.05f) {
                index = 4;   // desert
            }
            else if (temperature > 0.22f) {
                index = 7;   // savanna
            }

            result.biome = index;
            return result;
        }

        Blocks blocks_{};
        std::vector<Biome> biomes_;
        int min_y_ = -64;
        int section_count_ = 24;
        int sea_level_ = 63;
        std::uint32_t seed_ = 0x51ED2C3u;
        bool ready_ = false;
    };

} // namespace terila::world::terrain
