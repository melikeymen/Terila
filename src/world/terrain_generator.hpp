#pragma once

// The overworld generator: the same shape of world the game builds, in one pass.
//
// Vanilla drives terrain from a router of noise fields — continentalness, erosion, a ridge
// field for peaks — and paints the surface with per-biome rules, carving caves out of the
// result. This generator follows that model: every column has a climate (continent, erosion,
// temperature, humidity) which picks its biome and sets its height. The blocks below the
// surface come from the biome rules, caves and ores are carved in by their own helpers, and
// features (trees, cacti) are placed from hashes. A single block lookup and a whole chunk
// always agree, so the overlay only ever stores real player edits.

#include "world/chunk.hpp"
#include "world/block_cache.hpp"

#include <memory>
#include "world/light_engine.hpp"
#include "world/terrain_biomes.hpp"
#include "world/terrain_caves.hpp"
#include "world/terrain_noise.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace terila::world::terrain {

    // Deterministic heightmap + biome + cave generator. Chunk generation and single block
    // lookups share the same column and block functions, so an untouched block always equals
    // the generated one and the overlay only stores real edits.
    class Generator {
    public:
        // One column of the heightmap: the surface height and the biome that paints it.
        struct Column {
            int height = 0;
            int biome = 0;
        };

        // Columns are looked up hundreds of times per chunk (light propagation asks for the
        // sky above every cell of the halo), so the cache is a small direct-mapped table
        // instead of a hash map: one probe, no allocation. The climate nodes are cached on
        // top of that, which is where the noise cost actually lives.
        class ColumnCache {
        public:
            ColumnCache() {
                keys_.fill(kEmpty);
                cell_keys_.fill(kEmpty);
            }

            const Column* find(std::int32_t x, std::int32_t z) const {
                const std::size_t slot = slot_of(key(x, z));
                return keys_[slot] == key(x, z) ? &columns_[slot] : nullptr;
            }

            void insert(std::int32_t x, std::int32_t z, const Column& column) {
                const std::size_t slot = slot_of(key(x, z));
                keys_[slot] = key(x, z);
                columns_[slot] = column;
            }

            const Column* find_cell(std::int32_t cell_x, std::int32_t cell_z) const {
                const std::size_t slot = slot_of(key(cell_x, cell_z));
                return cell_keys_[slot] == key(cell_x, cell_z) ? &cells_[slot] : nullptr;
            }

            void insert_cell(std::int32_t cell_x, std::int32_t cell_z, const Column& column) {
                const std::size_t slot = slot_of(key(cell_x, cell_z));
                cell_keys_[slot] = key(cell_x, cell_z);
                cells_[slot] = column;
            }

        private:
            static constexpr std::uint64_t kEmpty = ~0ull;
            static constexpr std::size_t kSlots = 1024;

            static std::uint64_t key(std::int32_t x, std::int32_t z) {
                return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                    static_cast<std::uint32_t>(z);
            }

            static std::size_t slot_of(std::uint64_t key) {
                return static_cast<std::size_t>(detail::hash64(key)) & (kSlots - 1);
            }

            std::array<std::uint64_t, kSlots> keys_{};
            std::array<Column, kSlots> columns_{};
            std::array<std::uint64_t, kSlots> cell_keys_{};
            std::array<Column, kSlots> cells_{};
        };

        bool init(int min_y, int section_count, std::uint32_t seed,
            const IdResolver& block, const IdResolver& biome) {
            min_y_ = min_y;
            section_count_ = section_count;
            seed_ = seed;
            // A fresh table per world, salted with the seed: entries of an earlier world can
            // never be read back.
            block_cache_ = std::make_shared<BlockSampleCache>();
            cache_salt_ = static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 1;

            const auto resolved_blocks = resolve_blocks(block);
            if (!resolved_blocks) {
                return false;
            }
            blocks_ = *resolved_blocks;
            if (!resolve_biomes(biome, blocks_, biomes_)) {
                return false;
            }

            light_rules_ = light_rules_for(blocks_);
            init_noise();
            ready_ = true;
            return ready_;
        }

        bool ready() const { return ready_; }
        // What each block state does to light, so an edited chunk can be lit again.
        const LightRules& light_rules() const { return light_rules_; }
        std::uint32_t seed() const { return seed_; }
        int min_y() const { return min_y_; }
        int section_count() const { return section_count_; }
        int sea_level() const { return sea_level_; }

        // Highest solid block of a column, water and plants excluded.
        int surface_height(std::int32_t x, std::int32_t z) const {
            return column(x, z).height;
        }

        // Index into the biome table for a column, useful for diagnostics.
        int biome_at(std::int32_t x, std::int32_t z) const {
            return column(x, z).biome;
        }

        // Lookups without a caller-provided cache still reuse the last chunk's climate: a
        // player digging a block asks for one or two positions, and recomputing the noise of
        // the whole plant neighbourhood for each of them is pure waste.
        std::int32_t state_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            ColumnCache& cache = thread_cache();
            return state_at_with(x, y, z, [this, &cache](std::int32_t cx, std::int32_t cz) {
                return cached_column(cx, cz, cache);
            });
        }

        // The light engine asks for thousands of single blocks per chunk, each of which needs
        // the climate of its 5x5 plant neighbourhood, so the columns are cached for the
        // duration of one chunk.
        std::int32_t state_at(std::int32_t x, std::int32_t y, std::int32_t z,
            ColumnCache& cache) const {
            return state_at_with(x, y, z, [this, &cache](std::int32_t cx, std::int32_t cz) {
                return cached_column(cx, cz, cache);
            });
        }

        // The y of the block that keeps the sky out of a column: the surface, the water above
        // a flooded floor, or the highest leaf of a tree leaning into the column. Snow is
        // left out on purpose, it lets sunlight straight through. The light engine reads it
        // for the chunk and its whole 15 block halo, so it stays free of per-cell work.
        int sky_top(std::int32_t x, std::int32_t z) const {
            ColumnCache& cache = thread_cache();
            return sky_top_with(x, z, [this, &cache](std::int32_t cx, std::int32_t cz) {
                return cached_column(cx, cz, cache);
            });
        }

        int sky_top(std::int32_t x, std::int32_t z, ColumnCache& cache) const {
            return sky_top_with(x, z, [this, &cache](std::int32_t cx, std::int32_t cz) {
                return cached_column(cx, cz, cache);
            });
        }

        template<class ColumnFn>
        int sky_top_with(std::int32_t x, std::int32_t z, ColumnFn&& get_column) const {
            const Column col = get_column(x, z);
            int top = std::max(col.height, sea_level_ > col.height ? sea_level_ : col.height);

            for (std::int32_t tz = z - 2; tz <= z + 2; ++tz) {
                for (std::int32_t tx = x - 2; tx <= x + 2; ++tx) {
                    const Column plant = get_column(tx, tz);
                    const Biome& biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }
                    const int dx = x - tx;
                    const int dz = z - tz;
                    const int distance = std::abs(dx) + std::abs(dz);
                    const bool trunk_column = dx == 0 && dz == 0;

                    if (biome.tree_chance > 0 &&
                        detail::hash3(tx, tz, kFeatureSeed) % static_cast<std::uint32_t>(
                            biome.tree_chance) == 0) {
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
                    if (biome.cactus_chance > 0 &&
                        detail::hash3(tx, tz, kFeatureSeed + 2) % static_cast<std::uint32_t>(
                            biome.cactus_chance) == 0 && trunk_column) {
                        const int height = 1 + static_cast<int>(
                            detail::hash3(tx, tz, kFeatureSeed + 3) % 3);
                        top = std::max(top, plant.height + height);
                    }
                }
            }
            return top;
        }

        template<class ColumnFn>
        std::int32_t state_at_with(std::int32_t x, std::int32_t y, std::int32_t z,
            ColumnFn&& get_column) const {
            if (y < min_y_ || y >= min_y_ + section_count_ * 16) {
                return blocks_.air;
            }

            const Column col = get_column(x, z);
            const Biome& biome = biomes_[static_cast<std::size_t>(col.biome)];

            std::int32_t state = blocks_.air;
            if (y == min_y_) {
                state = blocks_.bedrock;
            }
            else if (y <= col.height) {
                // Caves are carved out of everything well under the surface.
                if (caves_.is_cave(x, y, z, col.height)) {
                    state = blocks_.air;
                }
                else if (y == col.height) {
                    state = biome.surface;
                }
                else if (y >= col.height - 3) {
                    state = biome.filler;
                }
                else {
                    state = ores_.replace_deep(x, y, z, biome.deep);
                }
            }
            else if (y <= sea_level_) {
                if (y == sea_level_ && biome.ice_top) {
                    state = blocks_.ice;
                }
                else {
                    state = blocks_.water;
                }
            }
            else if (y == col.height + 1 &&
                (biome.snow_cap || (biome.snow_above > 0 && col.height >= biome.snow_above))) {
                state = blocks_.snow;
            }

            return apply_plants(x, y, z, state, get_column);
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

            std::array<int, 256> column_biomes{};
            // One cache for the whole chunk: the fill, the features and the light pass all
            // ask for the same columns over and over.
            const auto cache = std::make_unique<ColumnCache>();

            // Terrain: the column decides where the ground is, the biome decides what it is
            // made of, and the caves hollow out everything that is well below the surface.
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    const std::int32_t wx = wx0 + lx;
                    const std::int32_t wz = wz0 + lz;
                    const Column col = column(wx, wz, *cache);
                    const Biome& biome = biomes_[static_cast<std::size_t>(col.biome)];
                    column_biomes[static_cast<std::size_t>(lz * 16 + lx)] = col.biome;

                    put(wx, min_y_, wz, blocks_.bedrock);
                    for (int y = min_y_ + 1; y <= col.height; ++y) {
                        if (caves_.is_cave(wx, y, wz, col.height)) {
                            continue;
                        }
                        std::int32_t state;
                        if (y == col.height) {
                            state = biome.surface;
                        }
                        else if (y >= col.height - 3) {
                            state = biome.filler;
                        }
                        else {
                            state = ores_.replace_deep(wx, y, wz, biome.deep);
                        }
                        put(wx, y, wz, state);
                    }
                    if (biome.snow_cap ||
                        (biome.snow_above > 0 && col.height >= biome.snow_above)) {
                        put(wx, col.height + 1, wz, blocks_.snow);
                    }
                    for (int y = col.height + 1; y <= sea_level_ && y > min_y_; ++y) {
                        put(wx, y, wz, (y == sea_level_ && biome.ice_top)
                            ? blocks_.ice : blocks_.water);
                    }
                }
            }

            // Trees and cacti: the margin lets plants from neighbouring columns lean in.
            for (std::int32_t tz = wz0 - 2; tz <= wz0 + 17; ++tz) {
                for (std::int32_t tx = wx0 - 2; tx <= wx0 + 17; ++tx) {
                    const Column plant = column(tx, tz, *cache);
                    const Biome& biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }

                    if (biome.tree_chance > 0 &&
                        detail::hash3(tx, tz, kFeatureSeed) % static_cast<std::uint32_t>(
                            biome.tree_chance) == 0) {
                        const int trunk = trunk_height(tx, tz);
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
                // A section holds a handful of states, mostly in long runs, so a one-entry
                // cache plus a linear scan of the palette beats a hash map probed 4096 times.
                std::vector<std::int32_t> palette;
                std::vector<std::int32_t> indices(4096, 0);
                std::int32_t last_state = -1;
                std::int32_t last_index = 0;
                int non_air = 0;
                int fluids = 0;

                for (int i = 0; i < 4096; ++i) {
                    const std::int32_t state = blocks[static_cast<std::size_t>(sy) * 4096 +
                        static_cast<std::size_t>(i)];
                    if (state != last_state) {
                        std::int32_t found = -1;
                        for (std::size_t k = 0; k < palette.size(); ++k) {
                            if (palette[k] == state) {
                                found = static_cast<std::int32_t>(k);
                                break;
                            }
                        }
                        if (found < 0) {
                            found = static_cast<std::int32_t>(palette.size());
                            palette.push_back(state);
                        }
                        last_state = state;
                        last_index = found;
                    }
                    indices[static_cast<std::size_t>(i)] = last_index;
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
                std::vector<std::int32_t> biome_palette;
                std::vector<std::int32_t> biome_indices(64, 0);
                for (int cy = 0; cy < 4; ++cy) {
                    for (int cz = 0; cz < 4; ++cz) {
                        for (int cxx = 0; cxx < 4; ++cxx) {
                            const int lx = std::min(15, cxx * 4 + 2);
                            const int lz = std::min(15, cz * 4 + 2);
                            const std::int32_t biome_id = biomes_[static_cast<std::size_t>(
                                column_biomes[static_cast<std::size_t>(lz * 16 + lx)])].id;
                            std::int32_t index = -1;
                            for (std::size_t k = 0; k < biome_palette.size(); ++k) {
                                if (biome_palette[k] == biome_id) {
                                    index = static_cast<std::int32_t>(k);
                                    break;
                                }
                            }
                            if (index < 0) {
                                index = static_cast<std::int32_t>(biome_palette.size());
                                biome_palette.push_back(biome_id);
                            }
                            biome_indices[static_cast<std::size_t>((cy * 4 + cz) * 4 + cxx)] =
                                index;
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

            // Real light propagation, not a column guess: sunlight falls from the sky and
            // spreads sideways into caves and under overhangs, water and leaves cost a level
            // per block, and every light source floods outwards. The neighbouring columns
            // come from the same generator, so the light along a chunk border matches what
            // the chunk on the other side computes.
            {
                // A halo cell costs a full block evaluation, and the four chunks that share
                // it would each pay for it again: the shared table makes the repeats free.
                const auto sample = [&](std::int32_t wx, std::int32_t wy, std::int32_t wz) {
                    if (!BlockSampleCache::is_enabled()) {
                        return state_at(wx, wy, wz, *cache);
                    }
                    const std::int64_t key = block_key(wx, wy, wz);
                    if (const auto hit = block_cache_->find(key, cache_salt_)) {
                        BlockSampleCache::count_hit();
                        return *hit;
                    }
                    BlockSampleCache::count_miss();
                    const std::int32_t state = state_at(wx, wy, wz, *cache);
                    block_cache_->insert(key, cache_salt_, state);
                    return state;
                };
                // A column's top is asked for again by every chunk that touches it, and each
                // answer walks a 5x5 tree neighbourhood: memoised in the same table, with the
                // unused top of the y range as its marker.
                const auto top = [this, &cache](std::int32_t wx, std::int32_t wz) {
                    if (!BlockSampleCache::is_enabled()) {
                        return sky_top(wx, wz, *cache);
                    }
                    const std::int64_t key = block_key(wx, kTopMarkerY, wz);
                    if (const auto hit = block_cache_->find(key, cache_salt_)) {
                        BlockSampleCache::count_hit();
                        return *hit;
                    }
                    BlockSampleCache::count_miss();
                    const int result = sky_top(wx, wz, *cache);
                    block_cache_->insert(key, cache_salt_, result);
                    return result;
                };
                LightEngine::compute(chunk, light_rules_, top, sample);
            }

            return chunk;
        }

    private:
        static constexpr std::uint32_t kFeatureSeed = 0x7E41A5u;
        // No column top can reach this height, so it marks a top entry in the sample table.
        static constexpr int kTopMarkerY = 2047;

        void init_noise() {
            // Every channel gets its own permutation, all derived from the world seed.
            const std::uint64_t base = static_cast<std::uint64_t>(seed_);
            continent_.init(base ^ 0xC0FFEEull, 4, 1.0f / 300.0f);
            erosion_.init(base ^ 0x0E05100ull, 4, 1.0f / 190.0f);
            ridge_.init(base ^ 0x21D6Eull, 3, 1.0f / 150.0f);
            temperature_.init(base ^ 0x7E11Eull, 3, 1.0f / 420.0f);
            humidity_.init(base ^ 0x4D1Dull, 3, 1.0f / 360.0f);
            detail_.init(base ^ 0xD7A1ull, 4, 1.0f / 64.0f);
            caves_.init(base, min_y_);
            ores_.init(base, min_y_, blocks_);
        }

        int trunk_height(std::int32_t x, std::int32_t z) const {
            return 4 + static_cast<int>(detail::hash3(x, z, kFeatureSeed + 1) % 3);
        }

        // One cache per thread for the lookups that do not bring their own, reset whenever a
        // generator with another seed starts using the thread.
        ColumnCache& thread_cache() const {
            struct Local {
                ColumnCache cache;
                std::uint32_t seed = 0;
            };
            static thread_local Local local;
            if (local.seed != seed_) {
                local.cache = ColumnCache{};
                local.seed = seed_;
            }
            return local.cache;
        }

        Column cached_column(std::int32_t cx, std::int32_t cz, ColumnCache& cache) const {
            if (const Column* found = cache.find(cx, cz)) {
                return *found;
            }
            const Column created = column(cx, cz, cache);
            cache.insert(cx, cz, created);
            return created;
        }

        // A column is the smooth blend of four climate cells, the way the game samples its
        // noise on a coarse grid and interpolates: four noise nodes per 16 columns instead of
        // sixteen, which is what makes a chunk cheap to build.
        Column column(std::int32_t x, std::int32_t z) const {
            const std::int32_t nx = x >> 2;
            const std::int32_t nz = z >> 2;
            const float fx = static_cast<float>(x & 3) * 0.25f;
            const float fz = static_cast<float>(z & 3) * 0.25f;

            const Column a = cell_column(nx, nz);
            if (fx == 0.0f && fz == 0.0f) {
                return a;
            }
            return blend(a, cell_column(nx + 1, nz), cell_column(nx, nz + 1),
                cell_column(nx + 1, nz + 1), fx, fz);
        }

        Column column(std::int32_t x, std::int32_t z, ColumnCache& cache) const {
            const std::int32_t nx = x >> 2;
            const std::int32_t nz = z >> 2;
            const float fx = static_cast<float>(x & 3) * 0.25f;
            const float fz = static_cast<float>(z & 3) * 0.25f;

            const Column a = cached_cell(nx, nz, cache);
            if (fx == 0.0f && fz == 0.0f) {
                return a;
            }
            return blend(a, cached_cell(nx + 1, nz, cache), cached_cell(nx, nz + 1, cache),
                cached_cell(nx + 1, nz + 1, cache), fx, fz);
        }

        // Heights blend smoothly between cells; the biome takes the nearest cell, so the
        // surface the player sees and the biome the client is told always agree.
        static Column blend(const Column& a, const Column& b, const Column& c,
            const Column& d, float fx, float fz) {
            const float top = static_cast<float>(a.height) +
                (static_cast<float>(b.height) - static_cast<float>(a.height)) * fx;
            const float bottom = static_cast<float>(c.height) +
                (static_cast<float>(d.height) - static_cast<float>(c.height)) * fx;
            const float mixed = top + (bottom - top) * fz;

            Column result;
            result.height = static_cast<int>(std::lround(mixed));
            if (fx >= 0.5f) {
                result.biome = fz >= 0.5f ? d.biome : b.biome;
            }
            else {
                result.biome = fz >= 0.5f ? c.biome : a.biome;
            }
            return result;
        }

        Column cached_cell(std::int32_t cell_x, std::int32_t cell_z, ColumnCache& cache) const {
            if (const Column* found = cache.find_cell(cell_x, cell_z)) {
                return *found;
            }
            const Column created = cell_column(cell_x, cell_z);
            cache.insert_cell(cell_x, cell_z, created);
            return created;
        }

        // The climate of one 4x4 cell, in the ranges the game uses: continentalness says how
        // far inland a place is, erosion how worn down the land is, and the ridge noise breaks
        // the peaks up. Together they decide the height and the biome.
        Column cell_column(std::int32_t cell_x, std::int32_t cell_z) const {
            const std::int32_t x = cell_x * 4;
            const std::int32_t z = cell_z * 4;
            const float fx = static_cast<float>(x);
            const float fz = static_cast<float>(z);

            const float continents = std::clamp(continent_.sample2(fx, fz) * 2.6f + 0.12f,
                -1.0f, 1.0f);
            const float erosion = std::clamp(erosion_.sample2(fx, fz) * 2.2f, -1.0f, 1.0f);
            const float temperature = std::clamp(temperature_.sample2(fx, fz) * 2.0f, -1.0f, 1.0f);
            const float humidity = std::clamp(humidity_.sample2(fx, fz) * 2.0f, -1.0f, 1.0f);
            const float ridge = 1.0f - std::abs(ridge_.sample2(fx, fz));
            const float detail = detail_.sample2(fx, fz);

            // Continentalness: sea floor, shelf, shore and the inland plateaus.
            float base;
            if (continents < -0.36f) {
                base = 26.0f + (continents + 1.0f) * 34.0f;
            }
            else if (continents < -0.12f) {
                base = 48.0f + (continents + 0.36f) * 58.0f;
            }
            else if (continents < 0.02f) {
                base = 62.0f + (continents + 0.12f) * 42.0f;
            }
            else {
                base = 67.0f + continents * 22.0f;
            }

            // Erosion sets how much relief the land has: worn-down plains at one end, jagged
            // mountains at the other.
            const float relief = std::clamp((0.35f - erosion) / 1.1f, 0.0f, 1.0f);
            // Peaks only rise where the ridge field is sharp *and* erosion is low, which is
            // what keeps mountains from covering the whole map.
            const float mountain = std::pow(std::max(0.0f, ridge - 0.3f), 1.4f) * relief * 130.0f;
            const float hills = relief * 14.0f + detail * (2.0f + relief * 9.0f);

            float height = base + mountain + hills;
            // Beaches stay close to the waterline instead of falling into a cliff.
            if (height > sea_level_ - 4.0f && height < sea_level_ + 3.0f) {
                height = std::clamp(height, static_cast<float>(sea_level_) - 3.0f,
                    static_cast<float>(sea_level_) + 2.0f);
            }

            const int max_y = min_y_ + section_count_ * 16 - 8;
            Column result;
            result.height = std::clamp(static_cast<int>(std::lround(height)), min_y_ + 3, max_y);
            result.biome = static_cast<int>(pick_biome(continents, erosion, temperature,
                humidity, result.height));
            return result;
        }

        std::size_t pick_biome(float continents, float erosion, float temperature,
            float humidity, int height) const {
            const bool cold = temperature < -0.45f;
            const bool chilly = temperature < -0.15f;
            const bool warm = temperature > 0.2f;
            const bool hot = temperature > 0.55f;
            const bool dry = humidity < -0.35f;
            const bool wet = humidity > 0.25f;

            if (continents < -0.36f) {
                return cold ? kFrozenOcean : kDeepOcean;
            }
            if (continents < -0.12f) {
                return cold ? kFrozenOcean : kOcean;
            }
            if (continents < 0.02f) {
                // The shore: bare rock where the coast is steep, sand where it is flat.
                if (height > sea_level_ + 4) {
                    return kStonyShore;
                }
                return (erosion < -0.45f && height > sea_level_ + 1) ? kStonyShore : kBeach;
            }

            if (height > 108) {
                return kStonyPeaks;
            }
            if (cold) {
                return wet ? kSnowyTaiga : kSnowyPlains;
            }
            if (chilly) {
                return wet ? kTaiga : kWindsweptHills;
            }
            if (height > sea_level_ + 30 && erosion < -0.5f) {
                return kWindsweptHills;
            }
            if (height <= sea_level_ + 2 && wet && !dry) {
                return kSwamp;
            }
            if (warm) {
                if (hot) {
                    return dry ? kBadlands : (wet ? kJungle : kSavanna);
                }
                if (dry) {
                    return kSavanna;
                }
                return wet ? kJungle : kPlains;
            }
            // Temperate: the forest band.
            if (dry) {
                return kPlains;
            }
            if (wet) {
                return kForest;
            }
            return (detail::hash3(0, height, kFeatureSeed + 9) & 1u) != 0
                ? kBirchForest : kMeadow;
        }

        template<class ColumnFn>
        std::int32_t apply_plants(std::int32_t x, std::int32_t y, std::int32_t z,
            std::int32_t state, ColumnFn&& get_column) const {
            // Plants only ever stand on land, and only reach a few blocks above it, so every
            // flood of underground and underwater lookups the light engine makes skips the
            // whole neighbourhood scan.
            if (y <= sea_level_ || y > sea_level_ + 48) {
                return state;
            }
            for (std::int32_t tz = z - 2; tz <= z + 2; ++tz) {
                for (std::int32_t tx = x - 2; tx <= x + 2; ++tx) {
                    const Column plant = get_column(tx, tz);
                    const Biome& plant_biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }
                    if (plant_biome.tree_chance > 0 &&
                        detail::hash3(tx, tz, kFeatureSeed) % static_cast<std::uint32_t>(
                            plant_biome.tree_chance) == 0) {
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

        Blocks blocks_{};
        std::vector<Biome> biomes_;
        LightRules light_rules_{};

        detail::OctaveNoise continent_;
        detail::OctaveNoise erosion_;
        detail::OctaveNoise ridge_;
        detail::OctaveNoise temperature_;
        detail::OctaveNoise humidity_;
        detail::OctaveNoise detail_;
        CaveCarver caves_;
        OreVeins ores_;

        int min_y_ = -64;
        int section_count_ = 24;
        int sea_level_ = 63;
        std::uint32_t seed_ = 0x51ED2C3u;
        // Every halo sample of every chunk goes through this, workers included: it is shared
        // state, so it is held by a pointer the generator hands to nobody else.
        mutable std::shared_ptr<BlockSampleCache> block_cache_ =
            std::make_shared<BlockSampleCache>();
        std::uint64_t cache_salt_ = 1;
        bool ready_ = false;
    };

}
