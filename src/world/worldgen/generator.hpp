#pragma once

// The overworld generator, driven by the game's own worldgen data.
//
// The shape of the land comes from the noise router and the density functions, the surface
// material from the material rule chain and the biomes from the multi-noise parameter list;
// this file only decides how to sample them. A column's height and biome are sampled on a
// 4x4 grid and interpolated, a chunk builds a 4x8 density grid for its caves, and every block
// lookup goes through the same two functions, so a streamed chunk and a single block read
// always agree.
//
// Deliberate simplifications, all of them documented where they matter: no structure
// blending or beardifier, no aquifers (the sea is the only water), no carvers (caves are the
// negative space of the density field), no cave biomes, and columns are heightmap shaped, so
// the field's overhangs above a column top are not streamed.

#include "world/block_cache.hpp"
#include "world/chunk.hpp"
#include "world/light_engine.hpp"
#include "world/terrain_biomes.hpp"
#include "world/terrain_noise.hpp"
#include "world/worldgen/material.hpp"
#include "world/worldgen/ores.hpp"
#include "world/worldgen/settings.hpp"
#include "world/worldgen/vegetation.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terila::world::worldgen {

    class Generator {
    public:
        // One column of the heightmap: the surface height, the biome that paints it and the
        // temperature the material rules test.
        struct Column {
            int height = 0;
            int biome = 0;
            float temperature = 0.0f;
        };

        // Columns are looked up hundreds of times per chunk (the light pass asks for the sky
        // above every cell of the halo), so the cache is a small direct-mapped table instead
        // of a hash map: one probe, no allocation.
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

            void insert_cell(std::int32_t cell_x, std::int32_t cell_z,
                const Column& column) {
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
                return static_cast<std::size_t>(terrain::detail::hash64(key)) & (kSlots - 1);
            }

            std::array<std::uint64_t, kSlots> keys_{};
            std::array<Column, kSlots> columns_{};
            std::array<std::uint64_t, kSlots> cell_keys_{};
            std::array<Column, kSlots> cells_{};
        };

        bool init(int min_y, int section_count, std::uint32_t seed,
            const terrain::IdResolver& block, const terrain::IdResolver& biome) {
            min_y_ = min_y;
            section_count_ = section_count;
            seed_ = seed;
            block_cache_ = std::make_shared<BlockSampleCache>();
            cache_salt_ = static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 1;

            const auto resolved_blocks = terrain::resolve_blocks(block);
            if (!resolved_blocks) {
                return false;
            }
            blocks_ = *resolved_blocks;
            if (!terrain::resolve_biomes(biome, blocks_, biomes_)) {
                return false;
            }
            for (std::size_t index = 0; index < biomes_.size(); ++index) {
                biome_index_[terrain::kBiomeNames[index]] = static_cast<int>(index);
            }

            if (!settings_.load(seed)) {
                LOG_WARN(Chunk, "terrain: the worldgen settings are missing or broken");
                return false;
            }
            if (settings_.section_count() != section_count_) {
                LOG_WARN(Chunk, "terrain: {} sections expected, the settings describe {}",
                    section_count_, settings_.section_count());
            }
            sea_level_ = settings_.sea_level();
            world_top_ = min_y_ + section_count_ * 16;
            cache_slots_ = settings_.density().cache_slot_count();

            if (!material_.load(settings_.material_rules(), settings_.material_conditions(),
                settings_.density(),
                [&block](std::string_view name) { return block(name); },
                [this](std::string_view name) -> std::optional<std::int32_t> {
                    const int index = biome_index_for(name);
                    if (index < 0) {
                        return std::nullopt;
                    }
                    return biomes_[static_cast<std::size_t>(index)].id;
                },
                settings_.material_rule_id(), min_y_)) {
                LOG_WARN(Chunk, "terrain: the material rules cannot be loaded");
                return false;
            }

            light_rules_ = terrain::light_rules_for(blocks_);
            ores_.init(static_cast<std::uint64_t>(seed_), min_y_, blocks_);
            plants_.init(blocks_, biomes_, sea_level_);
            ready_ = true;
            return ready_;
        }

        bool ready() const { return ready_; }
        std::uint32_t seed() const { return seed_; }
        int min_y() const { return min_y_; }
        int section_count() const { return section_count_; }
        int sea_level() const { return sea_level_; }
        const LightRules& light_rules() const { return light_rules_; }
        const Settings& settings() const { return settings_; }

        int surface_height(std::int32_t x, std::int32_t z) const {
            return column(x, z).height;
        }

        int biome_at(std::int32_t x, std::int32_t z) const {
            return column(x, z).biome;
        }

        // A single block, in the four kinds the world rules test.
        terrain::BlockKind kind_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            const std::int32_t state = state_at(x, y, z);
            if (state == blocks_.air) return terrain::BlockKind::Air;
            if (state == blocks_.water) return terrain::BlockKind::Water;
            if (state == blocks_.grass) return terrain::BlockKind::Soil;
            if (state == blocks_.snow) return terrain::BlockKind::Plant;
            for (const std::int32_t leaves : { blocks_.oak_leaves, blocks_.spruce_leaves,
                blocks_.birch_leaves, blocks_.jungle_leaves, blocks_.acacia_leaves }) {
                if (state == leaves) return terrain::BlockKind::Plant;
            }
            return terrain::BlockKind::Solid;
        }

        std::int32_t state_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            ColumnCache& cache = thread_cache();
            return state_at_with(x, y, z, [this, &cache](std::int32_t cx, std::int32_t cz) {
                return cached_column(cx, cz, cache);
            });
        }

        std::int32_t state_at(std::int32_t x, std::int32_t y, std::int32_t z,
            ColumnCache& cache) const {
            return state_at_with(x, y, z, [this, &cache](std::int32_t cx, std::int32_t cz) {
                return cached_column(cx, cz, cache);
            });
        }

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
            return plants_.sky_top(x, z, std::forward<ColumnFn>(get_column));
        }

        template<class ColumnFn>
        std::int32_t state_at_with(std::int32_t x, std::int32_t y, std::int32_t z,
            ColumnFn&& get_column) const {
            if (y < min_y_ || y >= world_top_) {
                return blocks_.air;
            }
            const Column col = get_column(x, z);
            std::int32_t state = block_at(x, y, z, col, nullptr);
            if (state == blocks_.air) {
                state = plants_.apply(x, y, z, state, get_column);
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
                if (y < min_y_ || y >= world_top_) {
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
            const auto cache = std::make_unique<ColumnCache>();

            // The density field of this chunk on the grid the game interpolates over: four
            // blocks horizontally, eight vertically. Caves come out of it; the surface itself
            // stays where the heightmap puts it.
            DensityGrid grid;
            build_grid(grid, wx0, wz0);

            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    const std::int32_t wx = wx0 + lx;
                    const std::int32_t wz = wz0 + lz;
                    const Column col = column(wx, wz, *cache);
                    const auto& biome = biomes_[static_cast<std::size_t>(col.biome)];
                    column_biomes[static_cast<std::size_t>(lz * 16 + lx)] = col.biome;

                    for (int y = min_y_; y <= std::min(col.height, world_top_ - 1); ++y) {
                        const std::int32_t state = block_at(wx, y, wz, col, &grid);
                        if (state != blocks_.air) {
                            put(wx, y, wz, state);
                        }
                    }
                    if (col.height >= sea_level_ && (biome.snow_cap ||
                        (biome.snow_above > 0 && col.height >= biome.snow_above))) {
                        put(wx, col.height + 1, wz, blocks_.snow);
                    }
                    for (int y = std::max(col.height + 1, min_y_);
                        y <= sea_level_ && y < world_top_; ++y) {
                        put(wx, y, wz, (y == sea_level_ && biome.ice_top)
                            ? blocks_.ice : blocks_.water);
                    }
                }
            }

            // Trees and cacti: the margin lets plants from neighbouring columns lean in.
            for (std::int32_t tz = wz0 - 2; tz <= wz0 + 17; ++tz) {
                for (std::int32_t tx = wx0 - 2; tx <= wx0 + 17; ++tx) {
                    const Column plant = column(tx, tz, *cache);
                    const auto& biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }

                    if (plants_.has_tree(biome, tx, tz)) {
                        const int trunk = plants_.trunk_height(tx, tz);
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

                    if (plants_.has_cactus(biome, tx, tz)) {
                        const int height = plants_.cactus_height(tx, tz);
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
                    for (int y = world_top_ - 1; y >= min_y_; --y) {
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
            for (int type : { 1, 4, 5 }) {
                Heightmap heightmap;
                heightmap.type_id = type;
                heightmap.longs = height_longs;
                chunk.heightmaps.push_back(std::move(heightmap));
            }

            chunk.sections.resize(static_cast<std::size_t>(section_count_));
            for (int sy = 0; sy < section_count_; ++sy) {
                ChunkSection& section = chunk.sections[static_cast<std::size_t>(sy)];
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

            {
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
        // No column top can reach this height, so it marks a top entry in the sample table.
        static constexpr int kTopMarkerY = 2047;

        struct DensityGrid {
            static constexpr int kCellsX = 5;
            static constexpr int kCellsZ = 5;
            int y_count = 0;
            int origin_x = 0;
            int origin_z = 0;
            int origin_y = 0;
            std::vector<double> values;

            void init(std::int32_t wx0, std::int32_t wz0, int min_y, int y_count_in) {
                origin_x = wx0;
                origin_z = wz0;
                origin_y = min_y;
                y_count = y_count_in;
                values.assign(static_cast<std::size_t>(kCellsX * kCellsZ * y_count), 0.0);
            }

            std::size_t index(int ix, int iy, int iz) const {
                return (static_cast<std::size_t>(iy) * kCellsZ + static_cast<std::size_t>(iz)) *
                    kCellsX + static_cast<std::size_t>(ix);
            }

            double at(std::int32_t x, std::int32_t y, std::int32_t z) const {
                const int ix = std::clamp((x - origin_x) >> 2, 0, kCellsX - 2);
                const int iz = std::clamp((z - origin_z) >> 2, 0, kCellsZ - 2);
                const int iy = std::clamp((y - origin_y) >> 3, 0, y_count - 2);
                const double tx = static_cast<double>(x - (origin_x + ix * 4)) / 4.0;
                const double tz = static_cast<double>(z - (origin_z + iz * 4)) / 4.0;
                const double ty = static_cast<double>(y - (origin_y + iy * 8)) / 8.0;

                const double c000 = values[index(ix, iy, iz)];
                const double c100 = values[index(ix + 1, iy, iz)];
                const double c010 = values[index(ix, iy, iz + 1)];
                const double c110 = values[index(ix + 1, iy, iz + 1)];
                const double c001 = values[index(ix, iy + 1, iz)];
                const double c101 = values[index(ix + 1, iy + 1, iz)];
                const double c011 = values[index(ix, iy + 1, iz + 1)];
                const double c111 = values[index(ix + 1, iy + 1, iz + 1)];

                const double bottom = (c000 + (c100 - c000) * tx) +
                    ((c010 + (c110 - c010) * tx) - (c000 + (c100 - c000) * tx)) * tz;
                const double top = (c001 + (c101 - c001) * tx) +
                    ((c011 + (c111 - c011) * tx) - (c001 + (c101 - c001) * tx)) * tz;
                return bottom + (top - bottom) * ty;
            }
        };

        static EvalContext& thread_context() {
            static thread_local EvalContext context;
            return context;
        }

        double sample_density(double x, double y, double z, EvalContext& context) const {
            return settings_.density().sample(settings_.final_density(), x, y, z, context);
        }

        // The density of one block, interpolated from the same 4x8 grid the chunk fill uses.
        double density_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            const std::int32_t x0 = (x >> 2) << 2;
            const std::int32_t z0 = (z >> 2) << 2;
            const std::int32_t y0 = min_y_ + (((y - min_y_) >> 3) << 3);
            const double tx = static_cast<double>(x - x0) / 4.0;
            const double tz = static_cast<double>(z - z0) / 4.0;
            const double ty = static_cast<double>(y - y0) / 8.0;

            EvalContext& context = thread_context();
            context.reset(cache_slots_);
            const double c000 = sample_density(x0, y0, z0, context);
            const double c100 = sample_density(x0 + 4, y0, z0, context);
            const double c010 = sample_density(x0, y0, z0 + 4, context);
            const double c110 = sample_density(x0 + 4, y0, z0 + 4, context);
            const double c001 = sample_density(x0, y0 + 8, z0, context);
            const double c101 = sample_density(x0 + 4, y0 + 8, z0, context);
            const double c011 = sample_density(x0, y0 + 8, z0 + 4, context);
            const double c111 = sample_density(x0 + 4, y0 + 8, z0 + 4, context);

            const double bottom = (c000 + (c100 - c000) * tx) +
                ((c010 + (c110 - c010) * tx) - (c000 + (c100 - c000) * tx)) * tz;
            const double top = (c001 + (c101 - c001) * tx) +
                ((c011 + (c111 - c011) * tx) - (c001 + (c101 - c001) * tx)) * tz;
            return bottom + (top - bottom) * ty;
        }

        void build_grid(DensityGrid& grid, std::int32_t wx0, std::int32_t wz0) const {
            grid.init(wx0, wz0, min_y_, (world_top_ - min_y_) / 8 + 1);
            EvalContext& context = thread_context();
            context.reset(cache_slots_);
            for (int iy = 0; iy < grid.y_count; ++iy) {
                for (int iz = 0; iz < DensityGrid::kCellsZ; ++iz) {
                    for (int ix = 0; ix < DensityGrid::kCellsX; ++ix) {
                        grid.values[grid.index(ix, iy, iz)] = sample_density(
                            wx0 + ix * 4, min_y_ + iy * 8, wz0 + iz * 4, context);
                    }
                }
            }
        }

        // The top solid block of a grid-aligned column, from the real density field.
        int density_top(std::int32_t x, std::int32_t z) const {
            EvalContext& context = thread_context();
            context.reset(cache_slots_);
            const int top = world_top_ - 8;
            int y = top;
            for (; y > min_y_; y -= 8) {
                if (sample_density(x, y, z, context) > 0.0) {
                    break;
                }
            }
            if (y <= min_y_) {
                return min_y_;
            }
            const double below = sample_density(x, y, z, context);
            const double above = sample_density(x, y + 8, z, context);
            if (above > 0.0 && y + 8 < world_top_) {
                // The whole cell is solid: keep the cell top, the surface is above it.
                return y + 8;
            }
            const double t = below / (below - above);
            return y + static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * 8.0));
        }

        std::int32_t block_at(std::int32_t x, std::int32_t y, std::int32_t z,
            const Column& col, const DensityGrid* grid) const {
            if (y < min_y_ || y >= world_top_) {
                return blocks_.air;
            }
            if (y == min_y_) {
                return blocks_.bedrock;
            }
            if (y <= col.height) {
                // Caves: the negative space of the density field, kept a few blocks away from
                // the surface so the material rules always have a top block to paint.
                if (y <= col.height - 3) {
                    const double density = grid != nullptr
                        ? grid->at(x, y, z) : density_at(x, y, z);
                    if (density < 0.0) {
                        return blocks_.air;
                    }
                }

                // The rule chain only has something to say near the surface: below ten
                // blocks it is the deep-slate gradient, which is the y < 0 test here. Asking
                // the chain once per deep block would be most of a chunk's generation time.
                std::int32_t state = -1;
                if (y >= col.height - 8) {
                    const auto& biome = biomes_[static_cast<std::size_t>(col.biome)];
                    MaterialContext context;
                    context.x = x;
                    context.y = y;
                    context.z = z;
                    context.surface_y = col.height;
                    context.stone_above = col.height - y;
                    context.stone_below = y - min_y_ + 1;
                    context.water_height = col.height < sea_level_
                        ? sea_level_ + 1 : std::numeric_limits<int>::min();
                    context.temperature = static_cast<double>(col.temperature);
                    context.biome = biome.id;
                    context.steepness = 0.0;
                    state = material_.apply(context);
                }
                if (state < 0) {
                    state = y < 0 && blocks_.deepslate != blocks_.stone
                        ? blocks_.deepslate : blocks_.stone;
                }
                if (state == blocks_.stone || state == blocks_.deepslate) {
                    state = ores_.replace_deep(x, y, z, state);
                }
                return state;
            }
            if (y <= sea_level_) {
                return (y == sea_level_ && biome_of(col).ice_top && col.height < sea_level_)
                    ? blocks_.ice : blocks_.water;
            }
            if (y == col.height + 1) {
                const auto& biome = biome_of(col);
                if (biome.snow_cap ||
                    (biome.snow_above > 0 && col.height >= biome.snow_above)) {
                    return blocks_.snow;
                }
            }
            return blocks_.air;
        }

        const terrain::Biome& biome_of(const Column& col) const {
            return biomes_[static_cast<std::size_t>(col.biome)];
        }

        // The climate cell of the 4x4 grid: the surface height from the density field and the
        // biome from the multi-noise parameters at the cell corner.
        Column cell_column(std::int32_t cell_x, std::int32_t cell_z) const {
            const std::int32_t x = cell_x * 4;
            const std::int32_t z = cell_z * 4;

            Column result;
            result.height = density_top(x, z);

            EvalContext& context = thread_context();
            context.reset(cache_slots_);
            const double y = static_cast<double>(sea_level_);
            const auto value = [&](const char* name) {
                const DfPtr function = settings_.root(name);
                return function ? settings_.density().sample(function, x, y, z, context)
                    : 0.0;
            };

            const double temperature = value("temperature");
            const double humidity = value("vegetation");
            const double continents = value("continents");
            const double erosion = value("erosion");
            // The parameter list separates surface biomes (depth 0) from cave biomes (1);
            // caves are painted by the density field here, so the surface is always depth 0.
            const double depth = 0.0;
            const double weirdness = value("ridges");

            const std::array<double, 6> climate{ temperature, humidity, continents, erosion,
                depth, weirdness };
            result.temperature = static_cast<float>(temperature);

            int index = -1;
            if (const std::string* name = settings_.climate().find(climate)) {
                index = biome_index_for(*name);
            }
            if (index < 0) {
                index = fallback_biome(temperature, humidity, continents);
            }
            result.biome = index;
            return result;
        }

        // Vanilla ships around sixty overworld biomes; the table here carries the ones the
        // spawn lists and the moon phases know. A climate box that names another one falls
        // back to the closest family.
        int biome_index_for(std::string_view name) const {
            const auto exact = biome_index_.find(std::string(name));
            if (exact != biome_index_.end()) {
                return exact->second;
            }
            const auto has = [name](std::string_view part) {
                return name.find(part) != std::string_view::npos;
            };
            if (has("ocean")) {
                return has("deep") ? static_cast<int>(terrain::kDeepOcean)
                    : (has("frozen") ? static_cast<int>(terrain::kFrozenOcean) : static_cast<int>(terrain::kOcean));
            }
            if (has("beach")) {
                return has("snowy") ? static_cast<int>(terrain::kSnowyPlains) : static_cast<int>(terrain::kBeach);
            }
            if (has("shore") || has("stony")) {
                return static_cast<int>(terrain::kStonyShore);
            }
            if (has("peaks")) {
                return static_cast<int>(terrain::kStonyPeaks);
            }
            if (has("swamp") || has("mangrove")) {
                return static_cast<int>(terrain::kSwamp);
            }
            if (has("jungle")) {
                return static_cast<int>(terrain::kJungle);
            }
            if (has("badlands")) {
                return static_cast<int>(terrain::kBadlands);
            }
            if (has("desert")) {
                return static_cast<int>(terrain::kDesert);
            }
            if (has("savanna")) {
                return static_cast<int>(terrain::kSavanna);
            }
            if (has("birch")) {
                return static_cast<int>(terrain::kBirchForest);
            }
            if (has("forest")) {
                return static_cast<int>(terrain::kForest);
            }
            if (has("taiga") || has("grove")) {
                return has("snowy") ? static_cast<int>(terrain::kSnowyTaiga) : static_cast<int>(terrain::kTaiga);
            }
            if (has("snowy") || has("frozen") || has("ice")) {
                return static_cast<int>(terrain::kSnowyPlains);
            }
            if (has("meadow")) {
                return static_cast<int>(terrain::kMeadow);
            }
            if (has("hills")) {
                return static_cast<int>(terrain::kWindsweptHills);
            }
            if (has("plains")) {
                return static_cast<int>(terrain::kPlains);
            }
            return -1;
        }

        static int fallback_biome(double temperature, double humidity, double continents) {
            if (continents < -0.2) {
                return static_cast<int>(terrain::kOcean);
            }
            if (temperature < -0.45) {
                return static_cast<int>(terrain::kSnowyPlains);
            }
            if (temperature < -0.15) {
                return static_cast<int>(terrain::kTaiga);
            }
            if (temperature > 0.55) {
                return humidity < -0.2 ? static_cast<int>(terrain::kDesert) : static_cast<int>(terrain::kSavanna);
            }
            if (temperature > 0.2) {
                return humidity > 0.2 ? static_cast<int>(terrain::kJungle) : static_cast<int>(terrain::kPlains);
            }
            return humidity > 0.2 ? static_cast<int>(terrain::kForest) : static_cast<int>(terrain::kMeadow);
        }

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

        static Column blend(const Column& a, const Column& b, const Column& c,
            const Column& d, float fx, float fz) {
            const float top = static_cast<float>(a.height) +
                (static_cast<float>(b.height) - static_cast<float>(a.height)) * fx;
            const float bottom = static_cast<float>(c.height) +
                (static_cast<float>(d.height) - static_cast<float>(c.height)) * fx;
            const float mixed = top + (bottom - top) * fz;

            Column result;
            result.height = static_cast<int>(std::lround(mixed));
            result.temperature = a.temperature + (b.temperature - a.temperature) * fx;
            result.temperature += (c.temperature + (d.temperature - c.temperature) * fx -
                result.temperature) * fz;
            if (fx >= 0.5f) {
                result.biome = fz >= 0.5f ? d.biome : b.biome;
            }
            else {
                result.biome = fz >= 0.5f ? c.biome : a.biome;
            }
            return result;
        }

        Column cached_cell(std::int32_t cell_x, std::int32_t cell_z,
            ColumnCache& cache) const {
            if (const Column* found = cache.find_cell(cell_x, cell_z)) {
                return *found;
            }
            const Column created = cell_column(cell_x, cell_z);
            cache.insert_cell(cell_x, cell_z, created);
            return created;
        }

        Settings settings_;
        MaterialRules material_;
        OreVeins ores_;
        Vegetation plants_;
        terrain::Blocks blocks_{};
        std::vector<terrain::Biome> biomes_;
        std::unordered_map<std::string, int> biome_index_;
        LightRules light_rules_{};

        int min_y_ = -64;
        int section_count_ = 24;
        int sea_level_ = 63;
        int world_top_ = 320;
        int cache_slots_ = 0;
        std::uint32_t seed_ = 0x51ED2C3u;
        mutable std::shared_ptr<BlockSampleCache> block_cache_ =
            std::make_shared<BlockSampleCache>();
        std::uint64_t cache_salt_ = 1;
        bool ready_ = false;
    };

} // namespace terila::world::worldgen
