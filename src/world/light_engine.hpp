#pragma once

#include "world/chunk.hpp"

#include <algorithm>
#include <cstdint>
#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terila::world {

    enum class LightLayer : std::uint8_t { Sky, Block };

    // The nibble a client would read out of a chunk's light arrays. The payloads are written
    // in section order and only for sections that carry light, so the array of a section is
    // found by counting the lit sections below it.
    inline int light_level_at(const Chunk& chunk, std::int32_t x, std::int32_t y,
        std::int32_t z, LightLayer layer) {
        const int local_y = y - chunk.min_y;
        const int height = chunk.section_count * 16;
        if (local_y < 0 || local_y >= height || x < 0 || x > 15 || z < 0 || z > 15) {
            return 0;
        }
        const auto& mask = layer == LightLayer::Sky ? chunk.light.sky_y_mask
                                                    : chunk.light.block_y_mask;
        const auto& updates = layer == LightLayer::Sky ? chunk.light.sky_updates
                                                       : chunk.light.block_updates;
        const auto lit = [&mask](int bit) {
            const auto word = static_cast<std::size_t>(bit / 64);
            return word < mask.size() && ((mask[word] >> (bit % 64)) & 1LL) != 0;
        };
        const int section = local_y / 16;
        if (!lit(section + 1)) {
            return 0;
        }
        std::size_t index = 0;
        for (int below = 1; below <= section; ++below) {
            if (lit(below)) ++index;
        }
        if (index >= updates.size()) {
            return 0;
        }
        const auto& array = updates[index];
        const int cell = ((local_y % 16) * 16 + z) * 16 + x;
        const auto byte = static_cast<std::size_t>(cell / 2);
        if (byte >= array.size()) {
            return 0;
        }
        return (cell & 1) == 0 ? (array[byte] & 0x0F) : (array[byte] >> 4);
    }

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

    // Light properties of every block state of this build, keyed by state id. The generator
    // asks the caller for this table instead of guessing: emission and dampening are read
    // from the game's own block data, so a window, a slab or a tuft of grass lets light
    // through the way it does in the game.
    using LightTable = std::unordered_map<std::int32_t, LightBlock>;

    inline LightRules rules_from(const LightTable& table) {
        LightRules rules;
        for (const auto& [state, block] : table) {
            rules.set(state, block);
        }
        return rules;
    }

    // Sky top of a column in a world that already carries player edits. The generator knows
    // the top of the untouched terrain; a block dug out of it lets the sky further down and a
    // block placed on top of what is left takes the sky away again, so the generated top is
    // only the starting point. A block floating above a gap is left to the sideways flood:
    // walking the whole column on every edit would cost more than the lighting itself.
    template<class GeneratedTopFn, class StateFn, class SkyDownFn>
    inline int edited_sky_top(int min_y, int world_top, std::int32_t x, std::int32_t z,
        GeneratedTopFn&& generated_top, StateFn&& state, SkyDownFn&& sky_down) {
        int top = std::clamp(generated_top(x, z), min_y - 1, world_top);
        while (top >= min_y && sky_down(state(x, top, z))) {
            --top;
        }
        while (top < world_top && !sky_down(state(x, top + 1, z))) {
            ++top;
        }

        return top;
    }

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

        // A tiny direct-mapped memo of state -> packed light properties: a chunk uses only a
        // handful of states, and every lookup otherwise walks the rules map.
        struct PropMemo {
            static constexpr std::size_t kSlots = 128;
            std::array<std::int32_t, kSlots> states{};
            std::array<std::uint16_t, kSlots> packed{};
            std::array<bool, kSlots> filled{};
        };

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

            // Block light comes from sources the terrain does not tell us about (torches a
            // player placed), so a world that has any keeps the whole height in the band.
            // A chunk that holds no emitter needs no block light pass, and keeping its band
            // as low as the sky allows is what keeps a generated chunk cheap: the terrain
            // itself never places a light source, only the player does.
            const bool sources = rules.has_emitters() && holds_emitter(chunk, rules);
            // Everything above the highest source is plain daylight, everything below the
            // lowest one by more than the reach of light is dark: the band is all the engine
            // has to look at. A lamp reaches above that line too, so a chunk that holds one
            // keeps the whole column.
            const int top_band = sources
                ? world_top
                : std::clamp(max_source, chunk.min_y, world_top);
            const int bottom_band = sources
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

            // The same handful of block states covers almost every cell of a chunk, and a
            // rules lookup is a map probe; this memo turns the flood fill's lookups into an
            // array index.
            PropMemo memo{};

            const auto pack = [&](std::int32_t state) -> std::uint16_t {
                const std::size_t slot = static_cast<std::size_t>(
                    (static_cast<std::uint32_t>(state) * 0x9E3779B1u) >> 16) &
                    (PropMemo::kSlots - 1);
                if (memo.filled[slot] && memo.states[slot] == state) {
                    return memo.packed[slot];
                }
                const LightBlock info = rules.of(state);
                const auto packed = static_cast<std::uint16_t>(1 | (info.opacity << 4) |
                    (info.emission << 8));
                memo.filled[slot] = true;
                memo.states[slot] = state;
                memo.packed[slot] = packed;
                return packed;
            };

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

                const std::int32_t state = (x >= 0 && x < 16 && z >= 0 && z < 16)
                    ? state_in_chunk(chunk, x, y, z)
                    : sample(chunk.x * 16 + x, y, chunk.z * 16 + z);
                slot = pack(state);
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

            // A flood fill visits every cell it lights once, so the queue is read forward and
            // never popped: a deque frees a block of cells on every pop_front, which costs
            // more than the lighting work the cells stand for.
            std::vector<int> queue_x;
            std::vector<int> queue_y;
            std::vector<int> queue_z;
            std::size_t queue_head = 0;

            const auto push = [&](int x, int y, int z) {
                queue_x.push_back(x);
                queue_y.push_back(y);
                queue_z.push_back(z);
            };

            const auto spread = [&](std::vector<std::uint8_t>& levels) {
                static constexpr int dx[6] = { 1, -1, 0, 0, 0, 0 };
                static constexpr int dy[6] = { 0, 0, 1, -1, 0, 0 };
                static constexpr int dz[6] = { 0, 0, 0, 0, 1, -1 };

                while (queue_head < queue_x.size()) {
                    const int x = queue_x[queue_head];
                    const int y = queue_y[queue_head];
                    const int z = queue_z[queue_head];
                    ++queue_head;

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

                // The next pass fills its own queue: the sky pass and the block light pass
                // each drain one.
                queue_x.clear();
                queue_y.clear();
                queue_z.clear();
                queue_head = 0;
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
            if (sources) {
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
        // A chunk's palette names every block state it holds, so whether it can light
        // anything is a handful of palette reads instead of a walk over its 98k blocks.
        static bool holds_emitter(const Chunk& chunk, const LightRules& rules) {
            for (const auto& section : chunk.sections) {
                // A section written with direct states does not name what it holds, so the
                // answer has to be the safe one and the pass runs.
                if (section.block_palette.empty() && !section.block_states.empty()) {
                    return true;
                }
                for (const std::int32_t state : section.block_palette) {
                    if (rules.of(state).emission > 0) {
                        return true;
                    }
                }
            }
            return false;
        }

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
                    // A chunk is taller than the band it had to look at: everything above it
                    // is open sky and everything below it is dark, so those sections are a
                    // fill instead of 4096 decisions.
                    if (base_y > top_band) {
                        if (is_sky) {
                            std::fill(packed.begin(), packed.end(), static_cast<std::uint8_t>(0xFF));
                            return std::pair{ true, std::move(packed) };
                        }
                        return std::pair{ false, std::move(packed) };
                    }
                    if (base_y + 15 < bottom_band) {
                        return std::pair{ false, std::move(packed) };
                    }

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
