#pragma once

#include "world/chunk.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terila::world {

    // The numbers the game reads from a block state for lighting: how much light it emits,
    // how much it removes (BlockBehaviour.getLightDampening, floored at one) and whether
    // sunlight passes straight down through it.
    struct LightBlock {
        std::uint8_t emission = 0;
        std::uint8_t opacity = 15;
        bool sky_down = false;
    };

    // Block states of this build mapped to their light properties. Anything not
    // registered counts as a solid block: no emission, fully opaque.
    class LightRules {
    public:
        void set(std::int32_t state, LightBlock block) {
            blocks_.insert_or_assign(state, block);
            if (block.emission > 0) {
                emits_ = true;
            }
        }

        LightBlock of(std::int32_t state) const {
            const auto it = blocks_.find(state);
            return it == blocks_.end() ? LightBlock{} : it->second;
        }

        // Terrain without a single light source skips the block light pass entirely.
        bool has_emitters() const { return emits_; }

    private:
        std::unordered_map<std::int32_t, LightBlock> blocks_;
        bool emits_ = false;
    };

    // Sky and block light of one chunk, the way the game computes it.
    //
    // Sunlight is not flooded down from the top: every column has one "sky source" height,
    // the lowest cell that still sees the open sky, and every cell at or above it is simply
    // full daylight. Only the cells where a column is shorter than its neighbour can send
    // light sideways, so only those are queued, and the flood then fills the shade under
    // trees, the dark of caves and the borders of the chunk. That keeps the work per chunk
    // proportional to what actually has to be darkened instead of its whole height.
    //
    // The halo: light of a neighbouring chunk can spill over the border, so the engine reads
    // a 15 block border around the chunk as well.
    class LightEngine {
    public:
        static constexpr int kRadius = 15;
        static constexpr int kMaxLevel = 15;

        // `sky_top(x, z)` is the y of the highest block of that column that keeps the sky
        // out (leaves, water, stone); `sample(x, y, z)` answers the block state of any
        // position, inside the chunk or in its halo.
        template<class SkyTopFn, class Sampler>
        static void compute(Chunk& chunk, const LightRules& rules, SkyTopFn&& sky_top,
            Sampler&& sample) {
            const int height = chunk.section_count * 16;
            if (height <= 0 || chunk.sections.empty()) {
                return;
            }
            const int world_top = chunk.min_y + height - 1;
            const int side = 16 + 2 * kRadius;

            // Sky source of every column of the chunk plus its halo.
            std::vector<int> source(static_cast<std::size_t>(side) * side, 0);
            const auto source_at = [&](int x, int z) -> int& {
                return source[static_cast<std::size_t>(z + kRadius) * side +
                    static_cast<std::size_t>(x + kRadius)];
            };

            int max_source = chunk.min_y;
            int min_source = world_top + 1;
            for (int z = -kRadius; z < 16 + kRadius; ++z) {
                for (int x = -kRadius; x < 16 + kRadius; ++x) {
                    const int top = std::clamp(
                        sky_top(chunk.x * 16 + x, chunk.z * 16 + z), chunk.min_y - 1,
                        world_top);
                    const int first_sky = top + 1;
                    source_at(x, z) = first_sky;
                    max_source = std::max(max_source, first_sky);
                    min_source = std::min(min_source, first_sky);
                }
            }

            // Everything above the highest source is plain daylight, everything below the
            // lowest one by more than the reach of light is dark: the band is all the engine
            // has to look at.
            const int top_band = std::clamp(max_source, chunk.min_y, world_top);
            // Block light comes from sources the terrain does not tell us about (torches a
            // player placed), so a world that has any keeps the whole height in the band.
            const int bottom_band = rules.has_emitters()
                ? chunk.min_y
                : std::clamp(min_source - kRadius - 2, chunk.min_y, world_top);
            if (bottom_band > top_band) {
                return;
            }

            const std::size_t cells = static_cast<std::size_t>(side) * side *
                static_cast<std::size_t>(top_band - bottom_band + 1);

            std::vector<std::uint8_t> sky(cells, 0);
            std::vector<std::uint8_t> block(cells, 0);
            // 0 means "not looked up yet"; the packed value stores opacity, emission and
            // the sky flag together.
            std::vector<std::uint16_t> props(cells, 0);

            const auto index_of = [&](int x, int y, int z) -> std::size_t {
                return (static_cast<std::size_t>(y - bottom_band) * side +
                    static_cast<std::size_t>(z + kRadius)) * side +
                    static_cast<std::size_t>(x + kRadius);
            };

            const auto inside = [&](int x, int y, int z) {
                return x >= -kRadius && x < 16 + kRadius && z >= -kRadius &&
                    z < 16 + kRadius && y >= bottom_band && y <= top_band;
            };

            constexpr std::uint16_t kSolid = static_cast<std::uint16_t>(1 | (15 << 4));
            constexpr std::uint16_t kOpenSky = static_cast<std::uint16_t>(1);

            const auto props_at = [&](int x, int y, int z) -> std::uint16_t {
                if (y > top_band) {
                    return kOpenSky;   // the band stops below every open sky column
                }
                if (!inside(x, y, z)) {
                    return kSolid;     // below the band, and outside the halo
                }
                std::uint16_t& slot = props[index_of(x, y, z)];
                if (slot != 0) {
                    return slot;
                }

                LightBlock info;
                if (x >= 0 && x < 16 && z >= 0 && z < 16) {
                    info = rules.of(state_in_chunk(chunk, x, y, z));
                }
                else {
                    info = rules.of(sample(chunk.x * 16 + x, y, chunk.z * 16 + z));
                }
                slot = static_cast<std::uint16_t>(1 | (info.opacity << 4) |
                    (info.emission << 8));
                return slot;
            };

            const auto opacity_of = [](std::uint16_t packed) {
                return static_cast<int>((packed >> 4) & 0x0F);
            };
            const auto emission_of = [](std::uint16_t packed) {
                return static_cast<int>((packed >> 8) & 0x0F);
            };
            // How much a level drops when it enters a cell, which is the game's getOpacity:
            // at least one, so even air costs a level once the light leaves its column. The
            // sun does not decay on the way down because whole columns are full daylight, not
            // because air is free.
            const auto cost = [&](std::uint16_t packed) {
                return std::max(1, opacity_of(packed));
            };

            std::deque<int> queue_x;
            std::deque<int> queue_y;
            std::deque<int> queue_z;

            const auto push = [&](int x, int y, int z) {
                queue_x.push_back(x);
                queue_y.push_back(y);
                queue_z.push_back(z);
            };

            const auto spread = [&](std::vector<std::uint8_t>& levels) {
                static constexpr int dx[6] = { 1, -1, 0, 0, 0, 0 };
                static constexpr int dy[6] = { 0, 0, 1, -1, 0, 0 };
                static constexpr int dz[6] = { 0, 0, 0, 0, 1, -1 };

                while (!queue_x.empty()) {
                    const int x = queue_x.front();
                    const int y = queue_y.front();
                    const int z = queue_z.front();
                    queue_x.pop_front();
                    queue_y.pop_front();
                    queue_z.pop_front();

                    const int level = levels[index_of(x, y, z)];
                    if (level <= 0) {
                        continue;
                    }

                    for (int i = 0; i < 6; ++i) {
                        const int nx = x + dx[i];
                        const int ny = y + dy[i];
                        const int nz = z + dz[i];
                        if (!inside(nx, ny, nz)) {
                            continue;
                        }
                        const int next = level - cost(props_at(nx, ny, nz));
                        if (next <= 0) {
                            continue;
                        }
                        const auto index = index_of(nx, ny, nz);
                        if (levels[index] >= next) {
                            continue;
                        }
                        levels[index] = static_cast<std::uint8_t>(next);
                        push(nx, ny, nz);
                    }
                }
            };

            // Sunlight: every cell of a column at or above its source is full daylight. The
            // cells that have a shorter neighbour are the only ones that can light anything
            // else, so only they enter the queue.
            for (int z = -kRadius; z < 16 + kRadius; ++z) {
                for (int x = -kRadius; x < 16 + kRadius; ++x) {
                    const int first_sky = source_at(x, z);
                    if (first_sky > top_band) {
                        continue;
                    }
                    // The halo edge has no neighbour of its own; the border column stands
                    // in for it, exactly like the game repeats the edge of its data.
                    const auto neighbour_at = [&](int nx, int nz) {
                        return source_at(std::clamp(nx, -kRadius, 16 + kRadius - 1),
                            std::clamp(nz, -kRadius, 16 + kRadius - 1));
                    };
                    const int neighbour = std::max(
                        std::max(neighbour_at(x - 1, z), neighbour_at(x + 1, z)),
                        std::max(neighbour_at(x, z - 1), neighbour_at(x, z + 1)));

                    for (int y = top_band; y >= std::max(first_sky, bottom_band); --y) {
                        sky[index_of(x, y, z)] = kMaxLevel;
                        if (y == first_sky || y < neighbour) {
                            push(x, y, z);
                        }
                    }
                }
            }
            spread(sky);

            // Block light: every emitter starts at its own level, then floods outwards.
            if (rules.has_emitters()) {
                for (int y = bottom_band; y <= top_band; ++y) {
                    for (int z = -kRadius; z < 16 + kRadius; ++z) {
                        for (int x = -kRadius; x < 16 + kRadius; ++x) {
                            const int emission = emission_of(props_at(x, y, z));
                            if (emission == 0) {
                                continue;
                            }
                            const auto index = index_of(x, y, z);
                            if (block[index] >= emission) {
                                continue;
                            }
                            block[index] = static_cast<std::uint8_t>(emission);
                            push(x, y, z);
                        }
                    }
                }
                spread(block);
            }

            write_light(chunk, sky, block, index_of, bottom_band, top_band);
        }

    private:
        static std::int32_t state_in_section(const ChunkSection& section, int index) {
            if (section.block_bits == 0) {
                return section.block_palette.empty() ? 0 : section.block_palette[0];
            }
            if (index < 0 || index >= static_cast<int>(section.block_states.size())) {
                return 0;
            }
            if (section.block_bits >= 9) {
                return section.block_states[static_cast<std::size_t>(index)];
            }
            const auto palette_index = section.block_states[static_cast<std::size_t>(index)];
            if (palette_index < 0 ||
                palette_index >= static_cast<std::int32_t>(section.block_palette.size())) {
                return 0;
            }
            return section.block_palette[static_cast<std::size_t>(palette_index)];
        }

        static std::int32_t state_in_chunk(const Chunk& chunk, int x, int y, int z) {
            const int local_y = y - chunk.min_y;
            if (local_y < 0 || x < 0 || x > 15 || z < 0 || z > 15) {
                return 0;
            }
            const auto section = static_cast<std::size_t>(local_y / 16);
            if (section >= chunk.sections.size()) {
                return 0;
            }
            return state_in_section(chunk.sections[section], ((local_y % 16) * 16 + z) * 16 + x);
        }

        // Packs the chunk into the section arrays the packet carries: one nibble array per
        // section that has light, the empty mask for the rest. Everything above the band is
        // open sky, everything below it is dark.
        template<class IndexFn>
        static void write_light(Chunk& chunk, const std::vector<std::uint8_t>& sky,
            const std::vector<std::uint8_t>& block, IndexFn&& index_of, int bottom_band,
            int top_band) {
            chunk.light = LightData{};
            const std::size_t mask_words =
                1 + (static_cast<std::size_t>(chunk.section_count) + 1) / 64;
            chunk.light.sky_y_mask.assign(mask_words, 0);
            chunk.light.block_y_mask.assign(mask_words, 0);
            chunk.light.empty_sky_y_mask.assign(mask_words, 0);
            chunk.light.empty_block_y_mask.assign(mask_words, 0);

            const auto set_bit = [](std::vector<std::int64_t>& mask, int bit) {
                mask[static_cast<std::size_t>(bit / 64)] |= (1LL << (bit % 64));
            };

            for (int sy = 0; sy < chunk.section_count; ++sy) {
                const int base_y = chunk.min_y + sy * 16;

                const auto build = [&](const std::vector<std::uint8_t>& levels, bool is_sky) {
                    std::vector<std::uint8_t> packed(2048, 0);
                    bool any = false;
                    for (int y = 0; y < 16; ++y) {
                        const int world_y = base_y + y;
                        for (int z = 0; z < 16; ++z) {
                            for (int x = 0; x < 16; ++x) {
                                int level = 0;
                                if (world_y > top_band) {
                                    level = is_sky ? kMaxLevel : 0;
                                }
                                else if (world_y >= bottom_band) {
                                    level = levels[index_of(x, world_y, z)];
                                }
                                if (level == 0) {
                                    continue;
                                }
                                any = true;
                                const int index = (y * 16 + z) * 16 + x;
                                const auto byte_index = static_cast<std::size_t>(index / 2);
                                if ((index & 1) == 0) {
                                    packed[byte_index] |= static_cast<std::uint8_t>(level & 0x0F);
                                }
                                else {
                                    packed[byte_index] |= static_cast<std::uint8_t>((level & 0x0F) << 4);
                                }
                            }
                        }
                    }
                    return std::pair{ any, std::move(packed) };
                };

                // Bit i of the masks addresses section i-1: bit 0 is the section below the
                // world, which is what the client expects.
                auto [sky_any, sky_array] = build(sky, true);
                if (sky_any) {
                    set_bit(chunk.light.sky_y_mask, sy + 1);
                    chunk.light.sky_updates.push_back(std::move(sky_array));
                }
                else {
                    set_bit(chunk.light.empty_sky_y_mask, sy + 1);
                }

                auto [block_any, block_array] = build(block, false);
                if (block_any) {
                    set_bit(chunk.light.block_y_mask, sy + 1);
                    chunk.light.block_updates.push_back(std::move(block_array));
                }
                else {
                    set_bit(chunk.light.empty_block_y_mask, sy + 1);
                }
            }
        }
    };

} // namespace terila::world
