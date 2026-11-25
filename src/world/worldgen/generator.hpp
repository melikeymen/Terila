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
// or beardifier, no carvers (caves are the negative space of the density field) and no
// features beyond the tree and cactus pass here.

#include "world/block_cache.hpp"
#include "world/chunk.hpp"
#include "world/light_engine.hpp"
#include "world/terrain_biomes.hpp"
#include "world/terrain_noise.hpp"
#include "world/worldgen/chunk_sections.hpp"
#include "world/worldgen/aquifer.hpp"
#include "world/worldgen/carver.hpp"
#include "world/worldgen/column.hpp"
#include "world/worldgen/density_grid.hpp"
#include "world/worldgen/dimension.hpp"
#include "world/worldgen/gpu/density_device.hpp"
#include "world/worldgen/settings.hpp"
#include "world/worldgen/surface/material.hpp"
#include "world/worldgen/structures/structures.hpp"
#include "world/worldgen/surface/ores.hpp"
#include "world/worldgen/feature/vegetation_plan.hpp"
#include "world/worldgen/surface/vegetation.hpp"
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
        bool init(int min_y, int section_count, std::uint32_t seed,
            const terrain::IdResolver& block, const terrain::IdResolver& biome,
            const StateTextResolver& state = {},
            DimensionKind kind = DimensionKind::Overworld,
            const StateRotator& rotate = {}) {
            min_y_ = min_y;
            section_count_ = section_count;
            seed_ = seed;
            kind_ = kind;
            dimension_ = dimension_type_of(kind);
            has_skylight_ = dimension_.has_skylight;
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

            // What grows on a biome comes from its own placed features, not from a table: the
            // densities, the species mix and the flower fields are read before the planter
            // copies the table.
            std::vector<StateProvider> providers;
            if (kind == DimensionKind::Overworld) {
                VegetationPlan vegetation;
                if (!vegetation.load(block)) {
                    LOG_WARN(Chunk, "terrain: the vegetation features cannot be read, the "
                        "biome table keeps its own numbers");
                }
                else {
                    vegetation.apply(biomes_);
                    providers = vegetation.providers();
                }
            }

            if (!settings_.load(seed, dimension_noise_preset(kind),
                    dimension_biome_preset(kind))) {
                LOG_WARN(Chunk, "terrain: the worldgen settings of {} are missing or broken",
                    dimension_.name);
                return false;
            }
            // A dimension is taller than its noise field (both the nether and the end are
            // 256 blocks high around a 128 block field), so the chunk height comes from the
            // dimension and the noise only decides where the ground is inside it.
            if (kind == DimensionKind::Overworld &&
                settings_.section_count() != section_count_) {
                LOG_WARN(Chunk, "terrain: {} sections expected, the settings describe {}",
                    section_count_, settings_.section_count());
            }
            sea_level_ = settings_.sea_level();
            fluid_ = block(settings_.default_fluid()).value_or(blocks_.air);
            deep_default_ = kind == DimensionKind::Nether ? blocks_.netherrack
                : (kind == DimensionKind::End ? blocks_.end_stone : blocks_.deepslate);
            world_top_ = min_y_ + section_count_ * 16;
            cache_slots_ = settings_.density().cache_slot_count();
            interp_slots_ = settings_.density().interp_slot_count();

            // The field of one chunk is a few thousand independent lattice points, so the
            // same tree can be walked on a device. The plan is built either way and the
            // backend only decides where the batch runs.
            // The plan owns the tables the device holds, so the last copy of this generator
            // gives them back instead of leaving them on the device for the rest of the run.
            gpu_plan_ = std::shared_ptr<gpu::DensityPlan>(
                new gpu::DensityPlan(gpu::build_plan(settings_.final_density())),
                [](gpu::DensityPlan* plan) {
                    gpu::DensityDevice::shared().forget(plan->id);
                    delete plan;
                });
            gpu_ready_ = gpu_plan_->usable() && gpu::use_device();
            if (gpu::use_device() && !gpu_plan_->usable()) {
                LOG_WARN(Chunk, "worldgen: the device path has no {} node, the chunk workers "
                    "keep the field", gpu_plan_->unsupported);
            }
            if (gpu_ready_ && gpu::backend() == gpu::Backend::Auto) {
                // The first generator on the process decides for all of them: the same tree
                // shape is measured once, on both sides, and the faster side keeps the work.
                gpu_ready_ = gpu::prefer_device_for(*gpu_plan_,
                    [this](double x, double y, double z) {
                        EvalContext& context = thread_context();
                        context.reset(cache_slots_);
                        return sample_density(x, y, z, context);
                    });
            }
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
            plants_.init(blocks_, biomes_, sea_level_, providers);
            // Ores and surface structures belong to the overworld's rule set; the nether and
            // the end have neither in this generator yet.
            if (kind_ == DimensionKind::Overworld) {
                if (!settings_.load_carvers([this](std::string_view name) -> int {
                        const auto found = biome_index_.find(std::string(name));
                        return found == biome_index_.end() ? -1 : found->second;
                    })) {
                    LOG_WARN(Chunk, "terrain: the carvers of this build cannot be read");
                }
                ores_.init(static_cast<std::uint64_t>(seed_), min_y_, world_top_ - 1, blocks_,
                    block);
                structures_.init(static_cast<std::uint64_t>(seed_), block,
                    [this](std::string_view name) -> int {
                        const auto found = biome_index_.find(std::string(name));
                        if (found == biome_index_.end()) {
                            return -1;
                        }
                        return biomes_[static_cast<std::size_t>(found->second)].id;
                    },
                    built_structures(), state, rotate);
            }
            ready_ = true;
            return ready_;
        }

        bool ready() const { return ready_; }
        DimensionKind kind() const { return kind_; }
        const DimensionType& dimension() const { return dimension_; }
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

        // The name the game gives one 4x4x4 cell. Terrain paints a column from the biome at its
        // surface; the cell a vein sits in belongs to the cave biome there, and the game names
        // that one by the nearest entry of its climate table.
        const std::string* biome_cell_name_at(std::int32_t x, int y, std::int32_t z) const {
            const ClimateSample climate = climate_sample(x, z);
            return settings_.climate().nearest(climate_point_at(x, y, z, climate));
        }

        // The biome of one block rather than of a column, which is what the decoration pass
        // asks: in the game's settings a cave has a biome of its own.
        const std::string* biome_name_at(std::int32_t x, int y, std::int32_t z) const {
            return biome_cell_name_at(x, y, z);
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
            if (!has_skylight_) {
                return min_y_ - 1;
            }
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
            // A roofed dimension never sees the sky, so nothing casts a shadow into it.
            if (!has_skylight_) {
                return min_y_ - 1;
            }
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
            if (state == blocks_.air) {
                state = ground_plant(x, y, z, col);
            }
            if (state == fluid_) {
                state = underwater_plant(x, y, z, col);
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
            thread_context().begin_chunk(interp_slots_);
            GridCache::Slot& field = chunk_field(cx, cz);
            const DensityGrid& grid = field.grid;
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
            std::array<unsigned char, 512> matter{};
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

                    // Rock, air or a fluid: the aquifer decides what the density leaves open,
                    // so a cave deep under the sea can hold water and one inside a mountain
                    // stays dry. A block the density fills never reaches the fluids.
                    for (int index = 0; index < height_count; ++index) {
                        const int y = min_y_ + index;
                        const Substance kind = field.aquifer.substance(wx, y, wz,
                            grid.at(wx, y, wz));
                        matter[static_cast<std::size_t>(index)] =
                            static_cast<unsigned char>(kind);
                        solid[static_cast<std::size_t>(index)] =
                            kind == Substance::Solid ? 1 : 0;
                    }

                    int stone_above = 0;
                    int water_height = std::numeric_limits<int>::min();
                    int next_ceiling = kWayBelow;

                    for (int y = world_top_ - 1; y >= min_y_; --y) {
                        const int index = y - min_y_;
                        const Substance kind =
                            static_cast<Substance>(matter[static_cast<std::size_t>(index)]);

                        std::int32_t state = blocks_.air;
                        if (kind == Substance::Solid) {
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
                        else if (kind == Substance::Water || kind == Substance::Lava) {
                            if (water_height == std::numeric_limits<int>::min()) {
                                water_height = y + 1;
                            }
                            // A frozen sea keeps its lid at the water line, on the first
                            // open block of the column.
                            state = kind == Substance::Water ? fluid_ : blocks_.lava;
                            if (kind == Substance::Water && y == sea_level_ - 1 &&
                                biome_of(col).ice_top) {
                                state = blocks_.ice;
                            }
                            if (state == fluid_) {
                                const std::int32_t plant = underwater_plant(wx, y, wz, col);
                                if (plant != blocks_.air) {
                                    state = plant;
                                }
                            }
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

            // Caves and canyons: grown from the eight chunk ring around this one, kept in a
            // mask, and then opened into whatever the aquifer says stands in the hole. Rock
            // that the barrier noise pushed back stays rock.
            if (!settings_.carvers().empty()) {
                MaskSlots& masks = thread_masks();
                MaskSlots::Slot& slot = masks.slots[masks.next];
                masks.next = (masks.next + 1) % MaskSlots::kSlots;
                slot.world = world_id_;
                slot.cx = cx;
                slot.cz = cz;
                slot.valid = true;
                slot.mask.begin(min_y_ + 1, world_top_ - 1 - 7);
                carve_into(slot.mask, cx, cz);
                if (timed) {
                    LOG_INFO(Chunk, "worldgen caves: {} blocks", slot.mask.carved_cells());
                }
                if (!slot.mask.empty()) {
                    for (int lz = 0; lz < 16; ++lz) {
                        for (int lx = 0; lx < 16; ++lx) {
                            const std::int32_t wx = wx0 + lx;
                            const std::int32_t wz = wz0 + lz;
                            for (int y = slot.mask.min_y(); y <= slot.mask.max_y(); ++y) {
                                if (!slot.mask.contains(lx, y, lz)) {
                                    continue;
                                }
                                const Substance kind = field.aquifer.substance(wx, y, wz, 0.0);
                                if (kind == Substance::Solid) {
                                    continue;
                                }
                                blocks[static_cast<std::size_t>(y - min_y_) * 256 +
                                    static_cast<std::size_t>(lz) * 16 +
                                    static_cast<std::size_t>(lx)] =
                                    kind == Substance::Water ? fluid_
                                        : (kind == Substance::Lava ? blocks_.lava : blocks_.air);
                            }
                        }
                    }
                }
                mark("carve");
            }

            // Structures land on the finished surface, before the vegetation stage that fills
            // the air above it.
            if (!structures_.empty()) {
                structures_.apply(cx, cz, sea_level_ + 1,
                    [this](std::int32_t x, std::int32_t z) { return surface_height(x, z); },
                    [this](std::int32_t x, std::int32_t z) {
                        const int index = biome_at(x, z);
                        return index >= 0 && index < static_cast<int>(biomes_.size())
                            ? biomes_[static_cast<std::size_t>(index)].id : -1;
                    },
                    [&put](std::int32_t x, std::int32_t y, std::int32_t z, std::int32_t state) {
                        put(x, y, z, state);
                    });
            }

            // The veins come after the carvers and before the plants, where the game runs its
            // decoration steps: a vein sees the caves around it, and a block read of this chunk
            // later answers the vein through the same overlay.
            if (kind_ == DimensionKind::Overworld) {
                ores_.decorate(cx, cz, blocks, sea_level_,
                    [this](std::int32_t x, int y, std::int32_t z) {
                        const std::string* name = biome_name_at(x, y, z);
                        return name == nullptr ? -1 : ores_.biome_id(*name);
                    });
            }

            // Trees and cacti first, so a canopy that leans into a neighbouring column keeps
            // its leaves; the ground cover below only fills what is still air. Placing the
            // cover first would leave a grass tuft inside another column's canopy, which a
            // single block read of that column (it asks the tree first) would deny.
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
                        const auto species = plants_.species_of(biome, tx, tz);
                        const int trunk = plants_.trunk_height(biome, tx, tz);
                        for (int i = 1; i <= trunk; ++i) {
                            put(tx, plant.height + i, tz, species.log);
                        }
                        for (int dy = trunk - 2; dy <= trunk + 1; ++dy) {
                            for (int dz = -2; dz <= 2; ++dz) {
                                for (int dx = -2; dx <= 2; ++dx) {
                                    const int distance = std::abs(dx) + std::abs(dz);
                                    if (distance > 3) continue;
                                    if (dx == 0 && dz == 0 && dy <= trunk) continue;
                                    if (dy == trunk + 1 && distance > 1) continue;
                                    put_if_air(tx + dx, plant.height + dy, tz + dz,
                                        species.leaves);
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

            // Ground cover, one block above the surface, and only on a column no tree or cactus
            // owns. It needs no margin: unlike a canopy it never reaches a neighbour. The
            // surface comes from the same place a single block read takes it, so the two land
            // on the same cell.
            for (std::int32_t tz = wz0; tz <= wz0 + 15; ++tz) {
                for (std::int32_t tx = wx0; tx <= wx0 + 15; ++tx) {
                    const Column plant = column(tx, tz, cache);
                    // A bank sits at the water line, so the surface is allowed to be exactly
                    // sea level: sugar cane grows there.
                    if (plant.height < sea_level_ || plant.height > sea_level_ + 40) {
                        continue;
                    }
                    // A tuft is one block, sugar cane up to three: ask for the whole band the
                    // plants can occupy, so a stalk is as tall in the chunk as a block read
                    // says it is.
                    for (int dy = 1; dy <= 3; ++dy) {
                        const std::int32_t ground = ground_plant(tx, plant.height + dy, tz, plant);
                        if (ground != blocks_.air) {
                            put_if_air(tx, plant.height + dy, tz, ground);
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

            // The packing itself lives in its own header: this stage only hands over the
            // block array and the biome of a position.
            write_sections(chunk, blocks, wx0, wz0, min_y_, section_count_, blocks_.air,
                blocks_.water,
                [&](std::int32_t bx, std::int32_t by, std::int32_t bz) {
                    const std::size_t column = static_cast<std::size_t>(
                        ((bz - wz0) / 4) * 4 + ((bx - wx0) / 4));
                    return biomes_[static_cast<std::size_t>(biome_at(bx, by, bz,
                        climate_columns[column]))].id;
                });

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
                if (!has_skylight_) {
                    std::fill(tops.begin(), tops.end(), min_y_ - 1);
                }
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

        // The field at one point, evaluated on the CPU: the round-trip tests compare this with
        // what the device answers for the same point.
        double density_at_point(double x, double y, double z) const {
            EvalContext& context = thread_context();
            context.reset(cache_slots_);
            return sample_density(x, y, z, context);
        }

        // True when the density field of this generator is evaluated on a device.
        bool device_backed() const noexcept { return gpu_ready_; }
        const gpu::DensityPlan& density_plan() const noexcept {
            static const gpu::DensityPlan empty;
            return gpu_plan_ ? *gpu_plan_ : empty;
        }

    private:
        // No column top can reach this height, so it marks a top entry in the sample table.
        static constexpr int kTopMarkerY = 2047;
        // A solid run that reaches the world floor reports a depth this far from the top.
        static constexpr int kWayBelow = -1000000;
        // The nether's own fluid level: below it the rock holds lava, above it nothing.
        static constexpr int kNetherLavaLevel = 32;
        // How far below the preliminary surface the material chain still paints blocks.
        static constexpr int kSurfaceWindow = 8;

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

        // The eight corner values of one interpolation cell. A block read walks a column, so a
        // few slots keep the walk from resampling the corners of the cell it stands in. The
        // slots outlive the generator, so each one carries the world it was filled from.
        struct DensityCells {
            static constexpr int kSlots = 5;

            struct Slot {
                std::uint64_t world = 0;
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

        // The interpolated field of a chunk, kept per thread. Only a chunk that is being
        // generated (or was just generated) has one: a lone block read samples its own cell
        // instead of paying for a whole chunk, which is what the game does as well.
        struct GridCache {
            static constexpr int kSlots = 9;

            struct Slot {
                std::uint64_t world = 0;
                int cx = 0;
                int cz = 0;
                bool valid = false;
                DensityGrid grid;
                Aquifer aquifer;
            };

            Slot slots[kSlots]{};
            int next = 0;
        };

        static GridCache& thread_grids() {
            static thread_local GridCache cache;
            return cache;
        }

        // A grid reference dies with the next build, so callers finish with one before asking
        // for another chunk.
        GridCache::Slot& chunk_field(int cx, int cz) const {
            GridCache& cache = thread_grids();
            for (auto& slot : cache.slots) {
                if (slot.valid && slot.world == world_id_ && slot.cx == cx && slot.cz == cz) {
                    return slot;
                }
            }
            GridCache::Slot& slot = cache.slots[cache.next];
            cache.next = (cache.next + 1) % GridCache::kSlots;
            slot.world = world_id_;
            slot.cx = cx;
            slot.cz = cz;
            slot.valid = true;
            build_grid(slot.grid, cx * 16, cz * 16);
            // The fluids of this chunk: the lattice is sized from the same volume the fill
            // walks, so the water a block read reports is the water the chunk holds.
            const FluidStatus bubble = kind_ == DimensionKind::Nether
                ? FluidStatus{ kNetherLavaLevel, Substance::Lava }
                : FluidStatus{ sea_level_, Substance::Water };
            slot.aquifer.init(&settings_.density(), settings_.aquifers(), seed_, cache_slots_,
                min_y_, world_top_ - 1, bubble);
            slot.aquifer.begin_chunk(cx * 16, cz * 16);
            // Columns of this chunk computed before its field existed carry heights from the
            // bare noise, which can sit a block away from the interpolated field the fill
            // paints from. They are dropped here and asked again.
            ColumnCache& columns = thread_cache();
            for (int dz = 0; dz < 16; ++dz) {
                for (int dx = 0; dx < 16; ++dx) {
                    columns.erase(cx * 16 + dx, cz * 16 + dz);
                }
            }
            for (int dz = 0; dz <= 4; ++dz) {
                for (int dx = 0; dx <= 4; ++dx) {
                    columns.erase_cell(cx * 16 + dx * 4, cz * 16 + dz * 4);
                }
            }
            return slot;
        }

        // The carved blocks of a chunk, kept per thread next to the field: a block read of a
        // generated chunk has to see the same air and water the file holds.
        struct MaskSlots {
            static constexpr int kSlots = 2;

            struct Slot {
                std::uint64_t world = 0;
                int cx = 0;
                int cz = 0;
                bool valid = false;
                CarvingMask mask;
            };

            Slot slots[kSlots]{};
            int next = 0;
        };

        static MaskSlots& thread_masks() {
            static thread_local MaskSlots masks;
            return masks;
        }

        const CarvingMask* cached_mask(std::int32_t x, std::int32_t z) const {
            const MaskSlots& masks = thread_masks();
            for (const auto& slot : masks.slots) {
                if (slot.valid && slot.world == world_id_ && slot.cx == (x >> 4) &&
                    slot.cz == (z >> 4)) {
                    return &slot.mask;
                }
            }
            return nullptr;
        }

        // The biome a carver start chunk belongs to, at the resolution the game asks it: the
        // noise biome of the chunk's corner cell at height zero, not a column of the terrain.
        int carver_biome(int cx, int cz) const {
            struct Slot {
                std::uint64_t world = 0;
                int cx = 0;
                int cz = 0;
                bool valid = false;
                int biome = 0;
            };
            static thread_local std::array<Slot, 2048> slots{};
            const std::uint64_t key = (static_cast<std::uint64_t>(world_id_) << 42) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 21) ^
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(cz));
            Slot& slot = slots[static_cast<std::size_t>(terrain::detail::hash64(key)) & 2047u];
            if (slot.valid && slot.world == world_id_ && slot.cx == cx && slot.cz == cz) {
                return slot.biome;
            }
            const std::int32_t x = cx * 16;
            const std::int32_t z = cz * 16;
            slot.world = world_id_;
            slot.cx = cx;
            slot.cz = cz;
            slot.valid = true;
            slot.biome = biome_at(x, 0, z, climate_sample(x, z));
            return slot.biome;
        }

        void carve_into(CarvingMask& mask, int cx, int cz) const {
            const Carvers& carvers = settings_.carvers();
            const int depth = world_top_ - min_y_;
            for (int dz = -8; dz <= 8; ++dz) {
                for (int dx = -8; dx <= 8; ++dx) {
                    const int source_x = cx + dx;
                    const int source_z = cz + dz;
                    const int biome = carver_biome(source_x, source_z);
                    const std::vector<int>& list = carvers.list_for_biome(biome);
                    for (std::size_t index = 0; index < list.size(); ++index) {
                        const ConfiguredCarver& carver = carvers.at(list[index]);
                        LegacyRandom random;
                        random.set_large_feature_seed(
                            seed_ + static_cast<std::uint64_t>(index), source_x, source_z);
                        if (random.next_float() <= carver.probability) {
                            Carvers::carve(carver, mask, cx, cz, source_x, source_z,
                                min_y_, depth, random);
                        }
                    }
                }
            }
        }

        GridCache::Slot* cached_field(std::int32_t x, std::int32_t z) const {
            GridCache& cache = thread_grids();
            for (auto& slot : cache.slots) {
                if (slot.valid && slot.world == world_id_ && slot.cx == (x >> 4) &&
                    slot.cz == (z >> 4)) {
                    return &slot;
                }
            }
            return nullptr;
        }

        const DensityGrid* cached_grid(std::int32_t x, std::int32_t z) const {
            GridCache& cache = thread_grids();
            for (const auto& slot : cache.slots) {
                if (slot.valid && slot.world == world_id_ && slot.cx == (x >> 4) &&
                    slot.cz == (z >> 4)) {
                    return &slot.grid;
                }
            }
            return nullptr;
        }

        // The density of one block. A chunk that is being generated answers from its own grid,
        // the way the fill sees it; anywhere else the eight corners of this block's cell are
        // sampled and blended with the same arithmetic, so the two can never disagree.
        double density_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            if (const DensityGrid* grid = cached_grid(x, z)) {
                return grid->at(x, y, z);
            }
            const std::int32_t x0 = (x >> 2) << 2;
            const std::int32_t z0 = (z >> 2) << 2;
            const std::int32_t y0 = min_y_ + (((y - min_y_) >> 3) << 3);
            const double tx = static_cast<double>(x - x0) / 4.0;
            const double tz = static_cast<double>(z - z0) / 4.0;
            const double ty = static_cast<double>(y - y0) / 8.0;

            DensityCells& cells = thread_cells();
            DensityCells::Slot* slot = nullptr;
            for (auto& candidate : cells.slots) {
                if (candidate.valid && candidate.world == world_id_ && candidate.x == x0 &&
                    candidate.y == y0 && candidate.z == z0) {
                    slot = &candidate;
                    break;
                }
            }
            if (slot == nullptr) {
                slot = &cells.slots[cells.next];
                cells.next = (cells.next + 1) % DensityCells::kSlots;
                EvalContext& context = thread_context();
                context.reset(cache_slots_);
                slot->world = world_id_;
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
            return blend_cell(slot->corners, tx, ty, tz);
        }

        void build_grid(DensityGrid& grid, std::int32_t wx0, std::int32_t wz0) const {
            grid.init(wx0, wz0, min_y_, (world_top_ - min_y_) / 8 + 1);
            if (device_grid(grid, wx0, wz0)) {
                return;
            }
            EvalContext& context = thread_context();
            context.reset(cache_slots_);
            for (int iy = 0; iy < grid.y_count; ++iy) {
                for (int iz = 0; iz < grid.cells_z; ++iz) {
                    for (int ix = 0; ix < grid.cells_x; ++ix) {
                        grid.values[grid.index(ix, iy, iz)] = sample_density(
                            wx0 + ix * 4, min_y_ + iy * 8, wz0 + iz * 4, context);
                    }
                }
            }
        }

        // The same grid points, handed to the device in one batch. The order is the grid's
        // own, so the values land in the array the CPU would have filled.
        bool device_grid(DensityGrid& grid, std::int32_t wx0, std::int32_t wz0) const {
            if (!gpu_ready_) {
                return false;
            }
            thread_local std::vector<gpu::GridPoint> points;
            thread_local std::vector<double> values;
            points.clear();
            points.reserve(grid.values.size());
            for (int iy = 0; iy < grid.y_count; ++iy) {
                for (int iz = 0; iz < grid.cells_z; ++iz) {
                    for (int ix = 0; ix < grid.cells_x; ++ix) {
                        points.push_back(gpu::GridPoint{ static_cast<double>(wx0 + ix * 4),
                            static_cast<double>(min_y_ + iy * 8),
                            static_cast<double>(wz0 + iz * 4) });
                    }
                }
            }
            if (!gpu::DensityDevice::shared().evaluate(*gpu_plan_, points, values) ||
                values.size() != grid.values.size()) {
                return false;
            }
            grid.values = values;
            return true;
        }

        // The highest solid block of a column, from the real density field.
        // The top of a column: walk down the interpolated field from the world roof and return
        // the first block the field calls solid, exactly the walk the game makes over a
        // column's density volume. Reading the grid keeps this arithmetic only, where sampling
        // the noise per block would cost a graph evaluation for every eight blocks of height.
        int density_top(std::int32_t x, std::int32_t z) const {
            // A chunk that was generated on this thread keeps its interpolated field around, so
            // its columns are answered from it, the same values the fill painted. Anywhere else
            // the field is sampled once per eight blocks of height, which is a single graph
            // evaluation per step instead of the eight corners a cell would cost.
            if (const DensityGrid* grid = cached_grid(x, z)) {
                for (int y = world_top_ - 1; y >= min_y_; y -= 8) {
                    if (grid->at(x, y, z) > 0.0) {
                        for (int cell = std::min(y + 7, world_top_ - 1); cell > y; --cell) {
                            if (grid->at(x, cell, z) > 0.0) {
                                return cell;
                            }
                        }
                        return y;
                    }
                }
                return min_y_;
            }
            EvalContext& context = thread_context();
            context.reset(cache_slots_);
            int start = world_top_ - 1;
            const DfPtr bounds = settings_.root("preliminary_surface_level");
            if (bounds) {
                // The router carries a cheap top surface estimate: the same field without the
                // jaggedness, walked by the graph itself. Only the field just above it has to be
                // walked here, and a column that turns out solid that high still falls back to
                // the full walk.
                const double level = settings_.density().sample(bounds, x, 0.0, z, context);
                const int ceiling = static_cast<int>(std::floor(level)) + 32;
                if (ceiling < start) {
                    start = std::max(ceiling, min_y_);
                }
            }
            for (int pass = 0; pass < 2; ++pass) {
                const int from = pass == 0 ? start : world_top_ - 1;
                for (int y = from; y >= min_y_; y -= 8) {
                    if (sample_density(x, y, z, context) > 0.0) {
                        for (int cell = std::min(y + 7, world_top_ - 1); cell > y; --cell) {
                            if (sample_density(x, cell, z, context) > 0.0) {
                                return cell;
                            }
                        }
                        return y;
                    }
                }
                if (start == world_top_ - 1) {
                    break;
                }
            }
            return min_y_;
        }

        // The game fades stone into deepslate over the eight blocks above y=0: a block there
        // is deepslate with a chance that falls from seven eighths at y=1 to nothing at y=8.
        std::int32_t deep_filler(std::int32_t x, std::int32_t y, std::int32_t z) const {
            // The nether and the end have no deepslate gradient: everything under the
            // surface is the dimension's own rock.
            if (kind_ != DimensionKind::Overworld) {
                return deep_default_;
            }
            // A decorated chunk keeps the veins the pass drew, so a block read of it answers
            // with those before this filler's own stone and deepslate.
            if (const std::int32_t vein = ores_.vein_at(x, y, z); vein != 0) {
                return vein;
            }
            if (blocks_.deepslate == blocks_.stone || y >= 8) {
                return blocks_.stone;
            }
            if (y <= 0) {
                return blocks_.deepslate;
            }
            const std::uint64_t hash = terrain::detail::hash64(
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 42) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) << 12) ^
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) ^ 0x6A09E667ull);
            return (hash % 8) < static_cast<std::uint64_t>(8 - y) ? blocks_.deepslate
                                                                 : blocks_.stone;
        }

        std::int32_t painted_at(std::int32_t x, std::int32_t y, std::int32_t z,
            const Column& col, int stone_above, int stone_below, int water_height) const {
            // Deep under the surface the chain can only answer the deepslate gradient, which
            // is the y test below; skipping the chain there keeps a chunk affordable.
            if (y + kSurfaceWindow < col.preliminary && y > min_y_ + 5) {
                return deep_filler(x, y, z);
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
            if (kind_ == DimensionKind::Overworld &&
                (state == blocks_.stone || state == blocks_.deepslate)) {
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
                state = fluid_at(x, y, z, col);
            }
            else if (y + kSurfaceWindow >= col.preliminary) {
                state = painted_at(x, y, z, col, 1, 1, water_height_of(col));
            }
            else {
                state = deep_filler(x, y, z);
            }
            if (state == blocks_.air) {
                state = plants_.apply(x, y, z, state,
                    [this, &cache](std::int32_t cx, std::int32_t cz) {
                        return cached_column(cx, cz, cache);
                    });
            }
            if (state == blocks_.air) {
                state = ground_plant(x, y, z, col);
            }
            if (state == fluid_) {
                state = underwater_plant(x, y, z, col);
            }
            return state;
        }

        // What grows under the sea: a patch of seagrass on the floor, or a kelp stalk that
        // climbs towards the surface. Which one, and how tall, comes from the column's own
        // coordinates, and the chunk fill and a single block read both ask this, so the ocean
        // looks the same whether it is streamed or read block by block.
        std::int32_t underwater_plant(std::int32_t x, std::int32_t y, std::int32_t z,
            const Column& col) const {
            if (kind_ != DimensionKind::Overworld || blocks_.seagrass == blocks_.air) {
                return blocks_.air;
            }
            if (col.biome != static_cast<int>(terrain::kDeepOcean) &&
                col.biome != static_cast<int>(terrain::kOcean) &&
                col.biome != static_cast<int>(terrain::kFrozenOcean)) {
                return blocks_.air;
            }
            // The floor has to be under the surface with at least one block of water over it.
            const int bottom = col.height + 1;
            if (bottom >= sea_level_ || y < bottom || y >= sea_level_) {
                return blocks_.air;
            }
            // Some columns carry a kelp stalk, most simply stand in a patch of seagrass.
            constexpr std::uint32_t kKelpSeed = 0x0CE1A9u;
            constexpr std::uint32_t kGrassSeed = 0x5EA6A55u;
            const std::uint32_t kelp = terrain::detail::hash3(x, z, kKelpSeed);
            if (kelp % 12 == 0 && blocks_.kelp != blocks_.air &&
                blocks_.kelp_plant != blocks_.air) {
                const int room = sea_level_ - bottom;
                const int height = 1 + static_cast<int>(kelp / 12 % static_cast<std::uint32_t>(room));
                const int top = bottom + height - 1;
                if (y > top) {
                    return blocks_.air;
                }
                // The last block of the stalk is the tip the client draws differently.
                return y == top ? blocks_.kelp : blocks_.kelp_plant;
            }
            if (y == bottom && terrain::detail::hash3(x, z, kGrassSeed) % 6 == 0) {
                return blocks_.seagrass;
            }
            return blocks_.air;
        }

        // The ground plant of a column, in the cell above its surface. The chunk fill and a
        // single block read both ask this, so the two cannot disagree about a tuft. The gates
        // are in the order that rejects the most first: a cell that is not the one right above
        // the surface needs no work at all, and most columns grow nothing.
        //
        // The plant has to stand on a block it can grow in, the way the game's would_survive
        // test does: a surface rule can paint stone inside a forest, and a tuft on that stone
        // would be a block nothing in the game would place.
        std::int32_t ground_plant(std::int32_t x, std::int32_t y, std::int32_t z,
            const Column& col) const {
            if (col.height < sea_level_ || col.height > sea_level_ + 40) {
                return blocks_.air;
            }
            // A tuft is one block tall, sugar cane up to three, so the band is generous and
            // each plant narrows it below.
            if (y < col.height + 1 || y > col.height + 3) {
                return blocks_.air;
            }
            const auto& biome = biomes_[static_cast<std::size_t>(col.biome)];
            if (plants_.has_tree(biome, x, z) || plants_.has_cactus(biome, x, z)) {
                return blocks_.air;
            }
            // A bank is rare and needs open water beside it, so the reed is asked first: the
            // ground cover of a wet biome is dense enough to take every column otherwise.
            const std::int32_t cane = sugar_cane_at(x, y, z, col);
            if (cane != blocks_.air) {
                return cane;
            }
            // Every ground plant stands in one cell, so the two cells above it are asked
            // before the provider of the cover is: a field sample per cell would be wasted
            // on a cell that cannot hold a plant anyway.
            if (y != col.height + 1) {
                return blocks_.air;
            }
            const std::int32_t plant = plants_.cover(biome, x, col.height, z);
            if (plant == blocks_.air) {
                return blocks_.air;
            }
            // The cell itself has to be air, and the one under it a block a plant grows in. A
            // blended column height can sit a cell above the ground, and without this the tuft
            // would hang over a pit.
            if (density_at(x, y, z) > 0.0) {
                return blocks_.air;
            }
            // A grass tuft or a flower needs soil a root can hold on to; the dead bush of a
            // desert also stands on sand, the way the game's would_survive test reads each.
            const std::int32_t ground = block_at(x, col.height, z, col);
            if (plant == blocks_.dead_bush ? !dry_plantable(ground) : !soil_plantable(ground)) {
                return blocks_.air;
            }
            return plant;
        }

        bool soil_plantable(std::int32_t state) const {
            return state == blocks_.grass || state == blocks_.dirt ||
                state == blocks_.coarse_dirt || state == blocks_.podzol ||
                state == blocks_.mycelium;
        }

        bool dry_plantable(std::int32_t state) const {
            return soil_plantable(state) || state == blocks_.sand ||
                state == blocks_.red_sand || state == blocks_.terracotta;
        }

        bool plantable(std::int32_t state) const {
            return dry_plantable(state);
        }

        // Sugar cane: a stalk one to three blocks tall on any bank next to open water, the way
        // the game grows it along rivers and shores rather than inside one biome. Like the
        // tufts it comes from the column's own hash, so a chunk and a block read agree.
        std::int32_t sugar_cane_at(std::int32_t x, std::int32_t y, std::int32_t z,
            const Column& col) const {
            if (blocks_.sugar_cane == blocks_.air) {
                return blocks_.air;
            }
            // Cheap rejections first: the hash decides most columns, before any neighbour is
            // asked about and before the ground is painted.
            if (y < col.height + 1 || y > col.height + 3) {
                return blocks_.air;
            }
            constexpr std::uint32_t kCaneSeed = 0x5ACA4Eu;
            constexpr std::uint32_t kCaneChance = 10;
            const std::uint32_t hash = terrain::detail::hash3(x, z, kCaneSeed);
            if (hash % kCaneChance != 0) {
                return blocks_.air;
            }
            // It only grows with water beside it: one of the four neighbours has to be flooded.
            ColumnCache& cache = thread_cache();
            const bool beside_water = cached_column(x + 1, z, cache).height < sea_level_ ||
                cached_column(x - 1, z, cache).height < sea_level_ ||
                cached_column(x, z + 1, cache).height < sea_level_ ||
                cached_column(x, z - 1, cache).height < sea_level_;
            if (!beside_water || density_at(x, y, z) > 0.0) {
                return blocks_.air;
            }
            if (!plantable(block_at(x, col.height, z, col))) {
                return blocks_.air;
            }
            const int height = 1 + static_cast<int>(hash % 3);
            return y <= col.height + height ? blocks_.sugar_cane : blocks_.air;
        }

        std::int32_t block_at(std::int32_t x, std::int32_t y, std::int32_t z,
            const Column& col) const {
            if (y < min_y_ || y >= world_top_) {
                return blocks_.air;
            }
            // The carvers open a block and the aquifer can push rock back over one, so the
            // field value alone is not the answer: rock, air or a fluid decides whether the
            // material rules are asked at all.
            if (matter_at(x, y, z) != Substance::Solid) {
                return fluid_at(x, y, z, col);
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

        // What stands in a block the density leaves open. A chunk that was generated on this
        // thread answers from its own aquifer, so the water a lookup reports is the water the
        // chunk holds; anywhere else the sea level rule stands in, which only differs in a
        // cave that the aquifer would have flooded.
        // Rock, air or a fluid, the same answer the fill of that chunk reached. A chunk that
        // was generated on this thread keeps its aquifer around; anywhere else the sea rule
        // stands in, which only differs inside a cave the aquifer would have flooded.
        Substance matter_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            if (const CarvingMask* mask = cached_mask(x, z)) {
                if (mask->contains(x & 15, y, z & 15)) {
                    if (GridCache::Slot* field = cached_field(x, z)) {
                        if (field->aquifer.ready()) {
                            return field->aquifer.substance(x, y, z, 0.0);
                        }
                    }
                    return Substance::Air;
                }
            }
            const double density = density_at(x, y, z);
            if (density > 0.0) {
                return Substance::Solid;
            }
            if (GridCache::Slot* field = cached_field(x, z)) {
                if (field->aquifer.ready()) {
                    return field->aquifer.substance(x, y, z, density);
                }
            }
            if (kind_ == DimensionKind::Nether) {
                return y < kNetherLavaLevel ? Substance::Lava : Substance::Air;
            }
            return y < sea_level_ ? Substance::Water : Substance::Air;
        }

        std::int32_t fluid_at(std::int32_t x, std::int32_t y, std::int32_t z,
            const Column& col) const {
            switch (matter_at(x, y, z)) {
            case Substance::Water:
                return (y == sea_level_ - 1 && biome_of(col).ice_top) ? blocks_.ice : fluid_;
            case Substance::Lava:
                return blocks_.lava;
            case Substance::Solid:
            case Substance::Air:
                return blocks_.air;
            }
            return blocks_.air;
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
            // The end picks its biome from the island noise and the distance to the centre,
            // which is what puts the central island, the barrens and the outer islands where
            // the game has them.
            if (kind_ == DimensionKind::End) {
                return end_biome_at(x, z);
            }
            std::array<double, 6> point{};
            int index = -1;
            if (const std::string* name = climate_name_at(x, y, z, climate, &point)) {
                index = biome_index_for(*name);
            }
            // The nether's parameter entries are single points, so a point that no box
            // contains goes to the nearest one, like the game's noise search does.
            if (index < 0 && kind_ == DimensionKind::Nether) {
                if (const std::string* nearest = settings_.climate().find_nearest(point)) {
                    index = biome_index_for(*nearest);
                }
            }
            if (index < 0) {
                index = fallback_biome(climate.temperature, climate.humidity,
                    climate.continents);
            }
            return std::max(0, index);
        }

        // The biome the climate point of this block names, before this checkout folds the name
        // into its smaller table. The decoration pass asks this one: an ore vein is gated on the
        // biome the game's own generation settings list it under, and underground that biome is
        // not the one the surface column shows.
        const std::string* climate_name_at(std::int32_t x, int y, std::int32_t z,
            const ClimateSample& climate,
            std::array<double, 6>* point_out = nullptr) const {
            if (kind_ == DimensionKind::End) {
                return nullptr;
            }
            const std::array<double, 6> point = climate_point_at(x, y, z, climate);
            if (point_out != nullptr) {
                *point_out = point;
            }
            return settings_.climate().find(point);
        }

        // The climate point of one block: the column's climate plus the depth the density
        // function reports at that height.
        std::array<double, 6> climate_point_at(std::int32_t x, int y, std::int32_t z,
            const ClimateSample& climate) const {
            double depth = 0.0;
            if (const DfPtr function = settings_.root("depth")) {
                EvalContext& context = thread_context();
                context.reset(cache_slots_);
                depth = settings_.density().sample(function, x, static_cast<double>(y), z,
                    context);
            }
            return { climate.temperature, climate.humidity, climate.continents, climate.erosion,
                depth, climate.weirdness };
        }

        // The end biome source: the central island is `the_end`, and the outer islands are
        // picked from the island noise in the bands the game uses.
        int end_biome_at(std::int32_t x, std::int32_t z) const {
            const std::int64_t dx = x;
            const std::int64_t dz = z;
            if (dx * dx + dz * dz <= 4096) {
                return static_cast<int>(terrain::kTheEnd);
            }
            double islands = 0.0;
            if (const DfPtr function = settings_.root("erosion")) {
                EvalContext& context = thread_context();
                context.reset(cache_slots_);
                islands = settings_.density().sample(function, x, 0.0, z, context);
            }
            if (islands > 0.25) {
                return static_cast<int>(terrain::kEndHighlands);
            }
            if (islands >= -0.0625) {
                return static_cast<int>(terrain::kEndMidlands);
            }
            if (islands <= -0.1875) {
                return static_cast<int>(terrain::kSmallEndIslands);
            }
            return static_cast<int>(terrain::kEndBarrens);
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
            Column mixed = blend(a, cached_cell(nx + 1, nz, cache),
                cached_cell(nx, nz + 1, cache), cached_cell(nx + 1, nz + 1, cache), fx, fz);
            // The blend smooths four cells into one column, which is what keeps a biome edge
            // from jumping, but the height is not something the game smooths: it walks this
            // column's own density. A blended height sits up to a cell away from the blocks
            // the chunk paints, and a plant or a surface rule that trusted it lands in the
            // wrong block.
            mixed.height = density_top(x, z);
            mixed.preliminary = mixed.height;
            return mixed;
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
            // The level the surface rules call ground has to be the height this column actually
            // has. Taking the nearest cell's preliminary instead left a blended column whose
            // height sat more than a surface window away from it with no surface rules at all,
            // so plains and forests came out as bare stone a cell above their own surface.
            result.preliminary = result.height;
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
        OreBlobs ores_;
        Structures structures_;
        Vegetation plants_;
        terrain::Blocks blocks_{};
        std::vector<terrain::Biome> biomes_;
        std::unordered_map<std::string, int> biome_index_;
        LightRules light_rules_{};
        const NormalNoise* surface_noise_ = nullptr;
        const NormalNoise* surface_secondary_noise_ = nullptr;

        DimensionKind kind_ = DimensionKind::Overworld;
        DimensionType dimension_{};
        bool has_skylight_ = true;
        std::int32_t fluid_ = 0;
        std::int32_t deep_default_ = 0;

        int min_y_ = -64;
        int section_count_ = 24;
        int sea_level_ = 63;
        int world_top_ = 320;
        int cache_slots_ = 0;
        int interp_slots_ = 0;
        std::shared_ptr<gpu::DensityPlan> gpu_plan_;
        bool gpu_ready_ = false;
        std::uint32_t seed_ = 0x51ED2C3u;
        mutable std::shared_ptr<BlockSampleCache> block_cache_ =
            std::make_shared<BlockSampleCache>();
        std::uint64_t cache_salt_ = 1;
        bool ready_ = false;
    };

} // namespace terila::world::worldgen
