#pragma once

#include "world/chunk.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace terila::world {

    struct BlockPos {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
    };

    inline std::int32_t section_state(const ChunkSection& sec, int index) {
        if (sec.block_bits == 0) {
            return sec.block_palette.empty() ? 0 : sec.block_palette[0];
        }
        if (index < 0 || index >= static_cast<int>(sec.block_states.size())) {
            return 0;
        }
        if (sec.block_bits >= 9) {
            return sec.block_states[static_cast<std::size_t>(index)];
        }
        const auto palette_index = sec.block_states[static_cast<std::size_t>(index)];
        if (palette_index < 0 || palette_index >= static_cast<std::int32_t>(sec.block_palette.size())) {
            return 0;
        }
        return sec.block_palette[static_cast<std::size_t>(palette_index)];
    }

    // Writers also have to grow the palette (and eventually go direct) when a
    // brand new block state shows up in an otherwise uniform section.
    inline void section_set(ChunkSection& sec, int index, std::int32_t state, std::int32_t air_state) {
        const std::int32_t old = section_state(sec, index);
        if (old == state) {
            return;
        }

        if (sec.block_bits == 0) {
            const std::int32_t base = sec.block_palette.empty() ? air_state : sec.block_palette[0];
            sec.block_bits = 4;
            sec.block_palette = { base, state };
            sec.block_states.assign(4096, 0);
            sec.block_states[static_cast<std::size_t>(index)] = 1;
        }
        else if (sec.block_bits >= 9) {
            sec.block_states[static_cast<std::size_t>(index)] = state;
        }
        else {
            const auto it = std::find(sec.block_palette.begin(), sec.block_palette.end(), state);
            if (it != sec.block_palette.end()) {
                sec.block_states[static_cast<std::size_t>(index)] =
                    static_cast<std::int32_t>(it - sec.block_palette.begin());
            }
            else {
                sec.block_palette.push_back(state);
                while (sec.block_bits < 9 &&
                       (1u << sec.block_bits) < sec.block_palette.size()) {
                    ++sec.block_bits;
                }

                if (sec.block_bits >= 9) {
                    // Too many states for an indirect palette: switch to direct values.
                    std::vector<std::int32_t> raw(sec.block_states.size(), 0);
                    for (std::size_t i = 0; i < sec.block_states.size(); ++i) {
                        const auto palette_index = sec.block_states[i];
                        if (palette_index >= 0 &&
                            palette_index < static_cast<std::int32_t>(sec.block_palette.size())) {
                            raw[i] = sec.block_palette[static_cast<std::size_t>(palette_index)];
                        }
                    }
                    raw[static_cast<std::size_t>(index)] = state;
                    sec.block_palette.clear();
                    sec.block_states = std::move(raw);
                }
                else {
                    sec.block_states[static_cast<std::size_t>(index)] =
                        static_cast<std::int32_t>(sec.block_palette.size() - 1);
                }
            }
        }

        if (old == air_state && state != air_state) {
            ++sec.block_count;
        }
        else if (old != air_state && state == air_state && sec.block_count > 0) {
            --sec.block_count;
        }
    }

    // Sparse overlay on top of the generated flat world. Only edited blocks are
    // stored; untouched chunks keep streaming straight from the shared template.
    class BlockWorld {
    public:
        static std::uint64_t chunk_key(std::int32_t cx, std::int32_t cz) {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
                static_cast<std::uint32_t>(cz);
        }

        bool set(const BlockPos& pos, std::int32_t state) {
            auto& entries = chunks_[chunk_key(pos.x >> 4, pos.z >> 4)];
            for (auto& entry : entries) {
                if (entry.pos.x == pos.x && entry.pos.y == pos.y && entry.pos.z == pos.z) {
                    entry.state = state;
                    return true;
                }
            }
            entries.push_back(Entry{ pos, state });
            return true;
        }

        std::int32_t get(const BlockPos& pos, std::int32_t fallback) const {
            const auto it = chunks_.find(chunk_key(pos.x >> 4, pos.z >> 4));
            if (it == chunks_.end()) {
                return fallback;
            }
            for (const auto& entry : it->second) {
                if (entry.pos.x == pos.x && entry.pos.y == pos.y && entry.pos.z == pos.z) {
                    return entry.state;
                }
            }
            return fallback;
        }

        bool chunk_modified(std::int32_t cx, std::int32_t cz) const {
            const auto it = chunks_.find(chunk_key(cx, cz));
            return it != chunks_.end() && !it->second.empty();
        }

        std::size_t modified_count(std::int32_t cx, std::int32_t cz) const {
            const auto it = chunks_.find(chunk_key(cx, cz));
            return it == chunks_.end() ? 0 : it->second.size();
        }

        void apply(Chunk& chunk, std::int32_t air_state) const {
            const auto it = chunks_.find(chunk_key(chunk.x, chunk.z));
            if (it == chunks_.end()) {
                return;
            }

            for (const auto& entry : it->second) {
                const std::int32_t local_y = entry.pos.y - chunk.min_y;
                if (local_y < 0 || local_y >= chunk.section_count * 16) {
                    continue;
                }

                const auto section_index = static_cast<std::size_t>(local_y / 16);
                if (section_index >= chunk.sections.size()) {
                    continue;
                }

                const int y = local_y % 16;
                const int z = entry.pos.z & 15;
                const int x = entry.pos.x & 15;
                const int index = (y * 16 + z) * 16 + x;
                section_set(chunk.sections[section_index], index, entry.state, air_state);
            }
        }

    private:
        struct Entry {
            BlockPos pos;
            std::int32_t state = 0;
        };

        std::unordered_map<std::uint64_t, std::vector<Entry>> chunks_;
    };

}
