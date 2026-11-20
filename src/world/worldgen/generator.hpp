#pragma once

// The overworld generator, driven by the game's own worldgen data.
//
// The shape of the land comes from the noise router and the density functions, the surface
// material from the material rule chain and the biomes from the multi-noise parameter list;
// this file only decides how to sample them. The density field is evaluated on the 4x8 cell
// grid the game interpolates over and every block is the sign of that field, so overhangs and
// cave systems fall out of the data instead of a heightmap. A single block lookup samples the
// same cell corners, so a streamed chunk and a block read always agree.
//
// Deliberate simplifications, all of them documented where they matter: no structure blending
// or beardifier, no aquifers (water fills the ocean and the caves below it, not local fluid
// levels), no carvers (caves are the negative space of the density field) and no features
// beyond the tree and cactus pass here.

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
#include <chrono>
#include <cmath>
#include <cstdlib>
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
        // values the material rules test.
        struct Column {
            int height = 0;
            int biome = 0;
            float temperature = 0.0f;
            int surface_depth = 3;
            double surface_secondary = 0.0;
            int preliminary = 0;
            // Memoised sky top: the light pass asks every cell of the halo for it.
            int sky = kNoSky;
            static constexpr int kNoSky = -1000000;
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

            void set_sky(std::int32_t x, std::int32_t z, int value) {
                const std::size_t slot = slot_of(key(x, z));
                if (keys_[slot] == key(x, z)) {
                    columns_[slot].sky = value;
                }
            }

            void insert_cell(std::int32_t cell_x, std::int32_t cell_z,
                const Column& column) {
                const std::size_t slot = slot_of(key(cell_x, cell_z));
                cell_keys_[slot] = key(cell_x, cell_z);
                cells_[slot] = column;
            }

        private:
            static constexpr std::uint64_t kEmpty = ~0ull;
            // The light pass asks for the sky of a fifteen block halo, so the table holds a
            // few thousand columns without evicting the coarser cells they are built from.
            static constexpr std::size_t kSlots = 8192;

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
            cell_cache_ = std::make_shared<CellCache<Column>>();
            world_id_ = next_world_id();
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
            interp_slots_ = settings_.density().interp_slot_count();
            surface_noise_ = settings_.density().noise("minecraft:surface");
            surface_secondary_noise_ =
                settings_.density().noise("minecraft:surface_secondary");

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
            terrain::add_placed_light_sources(light_rules_, block);
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
            ColumnCache& cache = thread_cache();
            return cached_column(x, z, cache).height;
        }

        int biome_at(std::int32_t x, std::int32_t z) const {
            ColumnCache& cache = thread_cache();
            return cached_column(x, z, cache).biome;
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
            if (const Column* hit = cache.find(x, z);
                hit != nullptr && hit->sky != Column::kNoSky) {
                return hit->sky;
            }
            const int value = sky_top_with(x, z, [this, &cache](std::int32_t cx, std::int32_t cz) {
                return cached_column(cx, cz, cache);
            });
            cache.set_sky(x, z, value);
            return value;
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
            std::int32_t state = block_at(x, y, z, col);
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
            const auto put_if_air = [&](std::int32_t wx, std::int32_t y, std::int32_t wz,
                std::int32_t state) {
                if (y < min_y_ || y >= world_top_) {
                    return;
                }
                const std::int32_t lx = wx - wx0;
                const std::int32_t lz = wz - wz0;
                if (lx < 0 || lx > 15 || lz < 0 || lz > 15) {
                    return;
                }
                std::int32_t& target = blocks[static_cast<std::size_t>(y - min_y_) * 256 +
                    static_cast<std::size_t>(lz) * 16 + static_cast<std::size_t>(lx)];
                if (target == blocks_.air) {
                    target = state;
                }
            };

            ColumnCache& cache = thread_cache();
            const bool timed = std::getenv("TERILA_WORLDGEN_TIME") != nullptr;
            auto stamp = std::chrono::steady_clock::now();
            const auto mark = [&](const char* stage) {
                if (!timed) {
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                LOG_INFO(Chunk, "worldgen {}: {:.1f} ms", stage,
                    std::chrono::duration<double, std::milli>(now - stamp).count());
                stamp = now;
            };

            // The density field of this chunk on the grid the game interpolates over: four
            // blocks horizontally, eight vertically.
            DensityGrid grid;
            thread_context().begin_chunk(interp_slots_);
            build_grid(grid, wx0, wz0);
            mark("grid");

            const auto solid_of = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
                return grid.at(x, y, z) > 0.0;
            };

            std::array<ClimateSample, 16> climate_columns{};
            for (int qz = 0; qz < 4; ++qz) {
                for (int qx = 0; qx < 4; ++qx) {
                    climate_columns[static_cast<std::size_t>(qz * 4 + qx)] =
                        climate_sample(wx0 + qx * 4 + 2, wz0 + qz * 4 + 2);
                }
            }

            std::array<unsigned char, 512> solid{};
            int height_sum = 0;
            int height_min = world_top_;
            int height_max = min_y_;
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    const std::int32_t wx = wx0 + lx;
                    const std::int32_t wz = wz0 + lz;
                    const Column col = column(wx, wz, cache);
                    height_sum += col.height;
                    height_min = std::min(height_min, col.height);
                    height_max = std::max(height_max, col.height);
                    const int height_count = world_height;

                    for (int index = 0; index < height_count; ++index) {
                        solid[static_cast<std::size_t>(index)] =
                            solid_of(wx, min_y_ + index, wz) ? 1 : 0;
                    }

                    // Water is the flooded aquifer state of the ocean: every empty block
                    // below sea level in a column whose floor is under the sea.
                    const bool flooded = col.height < sea_level_;
                    int stone_above = 0;
                    int water_height = std::numeric_limits<int>::min();
                    int next_ceiling = kWayBelow;

                    for (int y = world_top_ - 1; y >= min_y_; --y) {
                        const int index = y - min_y_;
                        const bool is_solid = solid[static_cast<std::size_t>(index)] != 0;
                        // Sea level is the first air block above the ocean: water ends one
                        // below it, exactly like the fluid level the game fills to.
                        const bool under_sea = y < sea_level_;

                        std::int32_t state = blocks_.air;
                        if (is_solid) {
                            if (next_ceiling >= y) {
                                next_ceiling = kWayBelow;
                                for (int below = y - 1; below >= min_y_; --below) {
                                    if (solid[static_cast<std::size_t>(below - min_y_)] == 0) {
                                        next_ceiling = below + 1;
                                        break;
                                    }
                                }
                            }
                            ++stone_above;
                            const int stone_below = y - next_ceiling + 1;
                            state = painted_at(wx, y, wz, col, stone_above, stone_below,
                                water_height);
                        }
                        else if (under_sea && flooded) {
                            if (water_height == std::numeric_limits<int>::min()) {
                                water_height = y + 1;
                            }
                            const auto& biome = biome_of(col);
                            state = (y == sea_level_ - 1 && biome.ice_top)
                                ? blocks_.ice : blocks_.water;
                        }
                        else {
                            stone_above = 0;
                            water_height = std::numeric_limits<int>::min();
                        }

                        if (state != blocks_.air) {
                            blocks[static_cast<std::size_t>(index) * 256 +
                                static_cast<std::size_t>(lz) * 16 +
                                static_cast<std::size_t>(lx)] = state;
                        }
                    }

                    // A cold surface keeps a snow layer on top of whatever the rules painted.
                    const auto& biome = biome_of(col);
                    if (biome.snow_cap ||
                        (biome.snow_above > 0 && col.height >= biome.snow_above)) {
                        put_if_air(wx, col.height + 1, wz, blocks_.snow);
                    }
                }
            }

            mark("fill");

            // Trees and cacti: the margin lets plants from neighbouring columns lean in.
            int tree_columns = 0;
            for (std::int32_t tz = wz0 - 2; tz <= wz0 + 17; ++tz) {
                for (std::int32_t tx = wx0 - 2; tx <= wx0 + 17; ++tx) {
                    const Column plant = column(tx, tz, cache);
                    const auto& biome = biomes_[static_cast<std::size_t>(plant.biome)];
                    if (plant.height <= sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }

                    if (plants_.has_tree(biome, tx, tz)) {
                        ++tree_columns;
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
                                    put_if_air(tx + dx, plant.height + dy, tz + dz,
                                        biome.leaves);
                                }
                            }
                        }
                        continue;
                    }

                    if (plants_.has_cactus(biome, tx, tz)) {
                        const int height = plants_.cactus_height(tx, tz);
                        for (int i = 1; i <= height; ++i) {
                            put_if_air(tx, plant.height + i, tz, blocks_.cactus);
                        }
                    }
                }
            }

            if (timed) {
                const Column center = column(wx0 + 8, wz0 + 8, cache);
                LOG_INFO(Chunk, "worldgen trees {} center biome {} chance {} height {} "
                    "avg {} min {} max {}", tree_columns, center.biome,
                    biomes_[static_cast<std::size_t>(center.biome)].tree_chance,
                    center.height, height_sum / 256, height_min, height_max);
            }
            mark("plants");

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

            mark("heightmaps");

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
                            const std::int32_t biome_id = biomes_[static_cast<std::size_t>(
                                biome_at(wx0 + cxx * 4 + 2, min_y_ + sy * 16 + cy * 4 + 2,
                                    wz0 + cz * 4 + 2,
                                    climate_columns[static_cast<std::size_t>(cz * 4 + cxx)]))].id;
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

            mark("sections");

            {
                const auto sample = [&](std::int32_t wx, std::int32_t wy, std::int32_t wz) {
                    if (!BlockSampleCache::is_enabled()) {
                        return halo_state_at(wx, wy, wz, cache);
                    }
                    const std::int64_t key = block_key(wx, wy, wz);
                    if (const auto hit = block_cache_->find(key, cache_salt_)) {
                        BlockSampleCache::count_hit();
                        return *hit;
                    }
                    BlockSampleCache::count_miss();
                    const std::int32_t state = halo_state_at(wx, wy, wz, cache);
                    block_cache_->insert(key, cache_salt_, state);
                    return state;
                };
                // The engine asks for one column top per halo column, 2116 of them, so the
                // whole halo is answered from one table instead of a scan per column.
                constexpr int kSide = 16 + 2 * LightEngine::kRadius;
                const std::int32_t hx0 = wx0 - LightEngine::kRadius;
                const std::int32_t hz0 = wz0 - LightEngine::kRadius;
                std::vector<int> tops;
                plants_.sky_tops(hx0, hz0, kSide,
                    [this, &cache](std::int32_t x, std::int32_t z) {
                        return cached_column(x, z, cache);
                    },
                    tops);
                mark("tops");
                const auto top = [this, &cache, &tops, hx0, hz0](std::int32_t wx,
                    std::int32_t wz) {
                    const std::int32_t tx = wx - hx0;
                    const std::int32_t tz = wz - hz0;
                    if (tx < 0 || tx >= kSide || tz < 0 || tz >= kSide) {
                        return sky_top(wx, wz, cache);
                    }
                    return tops[static_cast<std::size_t>(tz) * kSide +
                        static_cast<std::size_t>(tx)];
                };
                LightEngine::compute(chunk, light_rules_, top, sample);
            }
            mark("light");

            return chunk;
        }

    private:
        // No column top can reach this height, so it marks a top entry in the sample table.
        static constexpr int kTopMarkerY = 2047;
        // A solid run that reaches the world floor reports a depth this far from the top.
        static constexpr int kWayBelow = -1000000;
        // How far below the preliminary surface the material chain still paints blocks.
        static constexpr int kSurfaceWindow = 8;

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

        struct ClimateSample {
            double temperature = 0.0;
            double humidity = 0.0;
            double continents = 0.0;
            double erosion = 0.0;
            double weirdness = 0.0;
        };

        // One evaluation scratch per thread, tagged with this generator's id so two worlds
        // generated on the same thread never read each other's cached columns.
        EvalContext& thread_context() const {
            static thread_local EvalContext context;
            context.world = world_id_;
            return context;
        }

        static std::uint64_t next_world_id() {
            static std::atomic<std::uint64_t> counter{ 0 };
            return ++counter;
        }

        double sample_density(double x, double y, double z, EvalContext& context) const {
            return settings_.density().sample(settings_.final_density(), x, y, z, context);
        }

        double density(std::int32_t x, std::int32_t y, std::int32_t z) const {
            return density_at(x, y, z);
        }

        // The eight corner values of one interpolation cell. A block read walks up and down
        // a column, so five of these slots keep the light pass from resampling the corners of
        // the cell it is standing in.
        struct DensityCells {
            static constexpr int kSlots = 5;

            struct Slot {
                std::int32_t x = 0;
                std::int32_t y = 0;
                std::int32_t z = 0;
                bool valid = false;
                double corners[8]{};
            };

            Slot slots[kSlots]{};
            int next = 0;
        };

        static DensityCells& thread_cells() {
            static thread_local DensityCells cells;
            return cells;
        }

        // The density of one block, interpolated from the same 4x8 grid the chunk fill uses.
        double density_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            const std::int32_t x0 = (x >> 2) << 2;
            const std::int32_t z0 = (z >> 2) << 2;
            const std::int32_t y0 = min_y_ + (((y - min_y_) >> 3) << 3);
            const double tx = static_cast<double>(x - x0) / 4.0;
            const double tz = static_cast<double>(z - z0) / 4.0;
            const double ty = static_cast<double>(y - y0) / 8.0;

            DensityCells& cells = thread_cells();
            DensityCells::Slot* slot = nullptr;
            for (auto& candidate : cells.slots) {
                if (candidate.valid && candidate.x == x0 && candidate.y == y0 &&
                    candidate.z == z0) {
                    slot = &candidate;
                    break;
                }
            }
            if (slot == nullptr) {
                slot = &cells.slots[cells.next];
                cells.next = (cells.next + 1) % DensityCells::kSlots;
                EvalContext& context = thread_context();
                context.reset(cache_slots_);
                slot->x = x0;
                slot->y = y0;
                slot->z = z0;
                slot->valid = true;
                slot->corners[0] = sample_density(x0, y0, z0, context);
                slot->corners[1] = sample_density(x0 + 4, y0, z0, context);
                slot->corners[2] = sample_density(x0, y0, z0 + 4, context);
                slot->corners[3] = sample_density(x0 + 4, y0, z0 + 4, context);
                slot->corners[4] = sample_density(x0, y0 + 8, z0, context);
                slot->corners[5] = sample_density(x0 + 4, y0 + 8, z0, context);
                slot->corners[6] = sample_density(x0, y0 + 8, z0 + 4, context);
                slot->corners[7] = sample_density(x0 + 4, y0 + 8, z0 + 4, context);
            }

            const double c000 = slot->corners[0];
            const double c100 = slot->corners[1];
            const double c010 = slot->corners[2];
            const double c110 = slot->corners[3];
            const double c001 = slot->corners[4];
            const double c101 = slot->corners[5];
            const double c011 = slot->corners[6];
            const double c111 = slot->corners[7];
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

        // The highest solid block of a column, from the real density field.
        int density_top(std::int32_t x, std::int32_t z) const {
            EvalContext& context = thread_context();
            context.reset(cache_slots_);
            for (int y = world_top_ - 1; y >= min_y_; y -= 8) {
                if (sample_density(x, y, z, context) > 0.0) {
                    for (int cell = std::min(y + 7, world_top_ - 1); cell > y; --cell) {
                        if (sample_density(x, cell, z, context) > 0.0) {
                            return cell;
                        }
                    }
                    return y;
                }
            }
            return min_y_;
        }

        std::int32_t painted_at(std::int32_t x, std::int32_t y, std::int32_t z,
            const Column& col, int stone_above, int stone_below, int water_height) const {
            // Deep under the surface the chain can only answer the deepslate gradient, which
            // is the y test below; skipping the chain there keeps a chunk affordable.
            if (y + kSurfaceWindow < col.preliminary && y > min_y_ + 5) {
                return (y <= 0 && blocks_.deepslate != blocks_.stone)
                    ? blocks_.deepslate : blocks_.stone;
            }
            MaterialContext context;
            context.x = x;
            context.y = y;
            context.z = z;
            context.surface_y = col.height;
            context.stone_above = stone_above;
            context.stone_below = stone_below;
            context.water_height = water_height;
            context.surface_depth = col.surface_depth;
            context.surface_secondary = col.surface_secondary;
            context.temperature = static_cast<double>(col.temperature);
            context.biome = biome_of(col).id;
            context.steepness = 0.0;
            std::int32_t state = material_.apply(context);
            if (state < 0) {
                state = blocks_.stone;
            }
            if (state == blocks_.stone || state == blocks_.deepslate) {
                state = ores_.replace_deep(x, y, z, state);
            }
            return state;
        }

        // The light pass asks about the fifteen block halo around a chunk, where only the
        // surface layer is worth the material chain: the halo is not streamed, so its caves
        // are read as solid ground. Inside the chunk the light engine reads the finished
        // block array instead, so this never disagrees with a block the player can see.
        std::int32_t halo_state_at(std::int32_t x, std::int32_t y, std::int32_t z,
            ColumnCache& cache) const {
            if (y < min_y_ || y >= world_top_) {
                return blocks_.air;
            }
            const Column col = cached_column(x, z, cache);
            std::int32_t state = blocks_.air;
            if (y > col.height) {
                state = fluid_at(y, col);
            }
            else if (y + kSurfaceWindow >= col.preliminary) {
                state = painted_at(x, y, z, col, 1, 1, water_height_of(col));
            }
            else {
                state = (y <= 0 && blocks_.deepslate != blocks_.stone)
                    ? blocks_.deepslate : blocks_.stone;
            }
            if (state == blocks_.air) {
                state = plants_.apply(x, y, z, state,
                    [this, &cache](std::int32_t cx, std::int32_t cz) {
                        return cached_column(cx, cz, cache);
                    });
            }
            return state;
        }

        std::int32_t block_at(std::int32_t x, std::int32_t y, std::int32_t z,
            const Column& col) const {
            if (y < min_y_ || y >= world_top_) {
                return blocks_.air;
            }
            if (density(x, y, z) <= 0.0) {
                return fluid_at(y, col);
            }
            // Surface rules stop eight blocks below the preliminary surface, so a block deep
            // under it needs neither neighbour scan: the chain would answer stone or deepslate
            // whatever their depth is.
            const bool near_surface = y + kSurfaceWindow >= col.preliminary;
            if (!near_surface) {
                return painted_at(x, y, z, col, 1, 1, water_height_of(col));
            }
            int above = 0;
            while (above < kSurfaceWindow && y + above + 1 < world_top_ &&
                density(x, y + above + 1, z) > 0.0) {
                ++above;
            }
            int below = 0;
            while (below < kSurfaceWindow && y - below - 1 >= min_y_ &&
                density(x, y - below - 1, z) > 0.0) {
                ++below;
            }
            return painted_at(x, y, z, col, above + 1, below + 1, water_height_of(col));
        }

        // The material rules compare a block against the first water block of the column:
        // one above the sea surface when this column is flooded.
        int water_height_of(const Column& col) const {
            return col.height < sea_level_ ? sea_level_
                : std::numeric_limits<int>::min();
        }

        std::int32_t fluid_at(std::int32_t y, const Column& col) const {
            if (y >= sea_level_ || col.height >= sea_level_) {
                return blocks_.air;
            }
            return (y == sea_level_ - 1 && biome_of(col).ice_top) ? blocks_.ice : blocks_.water;
        }

        const terrain::Biome& biome_of(const Column& col) const {
            return biomes_[static_cast<std::size_t>(col.biome)];
        }

        ClimateSample climate_sample(std::int32_t x, std::int32_t z) const {
            ClimateSample result;
            EvalContext& context = thread_context();
            context.reset(cache_slots_);
            const double y = static_cast<double>(sea_level_);
            const auto value = [&](const char* name) {
                const DfPtr function = settings_.root(name);
                return function ? settings_.density().sample(function, x, y, z, context) : 0.0;
            };
            result.temperature = value("temperature");
            result.humidity = value("vegetation");
            result.continents = value("continents");
            result.erosion = value("erosion");
            result.weirdness = value("ridges");
            return result;
        }

        int biome_at(std::int32_t x, int y, std::int32_t z,
            const ClimateSample& climate) const {
            double depth = 0.0;
            if (const DfPtr function = settings_.root("depth")) {
                EvalContext& context = thread_context();
                context.reset(cache_slots_);
                depth = settings_.density().sample(function, x, static_cast<double>(y), z,
                    context);
            }
            const std::array<double, 6> point{ climate.temperature, climate.humidity,
                climate.continents, climate.erosion, depth, climate.weirdness };
            int index = -1;
            if (const std::string* name = settings_.climate().find(point)) {
                index = biome_index_for(*name);
            }
            if (index < 0) {
                index = fallback_biome(climate.temperature, climate.humidity,
                    climate.continents);
            }
            return std::max(0, index);
        }

        int surface_depth_of(std::int32_t x, std::int32_t z) const {
            if (surface_noise_ == nullptr) {
                return 3;
            }
            const double value = surface_noise_->sample(x, 0.0, z);
            return static_cast<int>(value * 2.75 + 3.0);
        }

        double surface_secondary_of(std::int32_t x, std::int32_t z) const {
            return surface_secondary_noise_ == nullptr
                ? 0.0 : surface_secondary_noise_->sample(x, 0.0, z);
        }

        // The level the surface rules treat as "ground". The game derives it from the same
        // density field by walking down a coarse grid; asking the field's own top-surface
        // function per column would cost several interpolated volumes for one integer, so the
        // walk is shared with the height lookup.
        int preliminary_surface_of(std::int32_t x, std::int32_t z) const {
            return density_top(x, z);
        }

        // The climate cell of the 4x4 grid: the surface height from the density field, the
        // biome and surface rules sampled at the surface.
        Column cell_column(std::int32_t cell_x, std::int32_t cell_z) const {
            const std::int32_t x = cell_x * 4;
            const std::int32_t z = cell_z * 4;

            Column result;
            result.height = density_top(x, z);
            const ClimateSample climate = climate_sample(x, z);
            result.temperature = static_cast<float>(climate.temperature);
            result.biome = biome_at(x, result.height + 1, z, climate);
            result.surface_depth = surface_depth_of(x, z);
            result.surface_secondary = surface_secondary_of(x, z);
            result.preliminary = result.height;
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
            const Column& nearest = fx >= 0.5f
                ? (fz >= 0.5f ? d : b)
                : (fz >= 0.5f ? c : a);
            result.biome = nearest.biome;
            result.surface_depth = nearest.surface_depth;
            result.surface_secondary = nearest.surface_secondary;
            result.preliminary = nearest.preliminary;
            return result;
        }

        // Cell coordinates are a quarter of the block ones, so the same 32 bit packing fits.
        static std::int64_t cell_key(std::int32_t cell_x, std::int32_t cell_z) noexcept {
            return static_cast<std::int64_t>(
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell_x)) << 32) |
                static_cast<std::uint32_t>(cell_z));
        }

        Column cached_cell(std::int32_t cell_x, std::int32_t cell_z,
            ColumnCache& cache) const {
            if (const Column* found = cache.find_cell(cell_x, cell_z)) {
                return *found;
            }
            const std::int64_t key = cell_key(cell_x, cell_z);
            if (cell_cache_ != nullptr) {
                if (const auto shared = cell_cache_->find(key, cache_salt_)) {
                    cache.insert_cell(cell_x, cell_z, *shared);
                    return *shared;
                }
            }
            const Column created = cell_column(cell_x, cell_z);
            if (cell_cache_ != nullptr) {
                cell_cache_->insert(key, cache_salt_, created);
            }
            cache.insert_cell(cell_x, cell_z, created);
            return created;
        }

        Settings settings_;
        std::shared_ptr<CellCache<Column>> cell_cache_;
        std::uint64_t world_id_ = 0;

        MaterialRules material_;
        OreVeins ores_;
        Vegetation plants_;
        terrain::Blocks blocks_{};
        std::vector<terrain::Biome> biomes_;
        std::unordered_map<std::string, int> biome_index_;
        LightRules light_rules_{};
        const NormalNoise* surface_noise_ = nullptr;
        const NormalNoise* surface_secondary_noise_ = nullptr;

        int min_y_ = -64;
        int section_count_ = 24;
        int sea_level_ = 63;
        int world_top_ = 320;
        int cache_slots_ = 0;
        int interp_slots_ = 0;
        std::uint32_t seed_ = 0x51ED2C3u;
        mutable std::shared_ptr<BlockSampleCache> block_cache_ =
            std::make_shared<BlockSampleCache>();
        std::uint64_t cache_salt_ = 1;
        bool ready_ = false;
    };

} // namespace terila::world::worldgen
