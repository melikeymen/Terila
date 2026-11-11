#pragma once

#include "world/chunk.hpp"
#include "protocol/varint.hpp"
#include "log/logger.hpp"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

namespace terila::world::decode {

    namespace detail {

        inline std::int64_t read_i64_be(const std::uint8_t* p) {
            std::uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
            return static_cast<std::int64_t>(v);
        }

        inline std::int32_t read_i32_be(const std::uint8_t* p) {
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v = (v << 8) | p[i];
            return static_cast<std::int32_t>(v);
        }

        inline std::uint16_t read_u16_be(const std::uint8_t* p) {
            return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
        }

        inline bool decode_palette_container(
            const std::uint8_t* data, std::size_t size, std::size_t& pos,
            int entry_count, int min_bits_direct,
            std::uint8_t& out_bits,
            std::vector<std::int32_t>& out_palette,
            std::vector<std::int32_t>& out_states)
        {
            if (pos >= size) return false;
            out_bits = data[pos++];
            out_palette.clear();
            out_states.clear();

            if (out_bits == 0) {
                auto vr = terila::protocol::read_varint(data + pos, size - pos);
                if (!vr) return false;
                pos += vr->consumed;
                out_palette.push_back(vr->value);
                out_states.assign(entry_count, 0);
                return true;
            }

            const bool is_direct = out_bits >= min_bits_direct;

            if (!is_direct) {
                auto pl = terila::protocol::read_varint(data + pos, size - pos);
                if (!pl || pl->value < 0 || pl->value > 4096) return false;
                pos += pl->consumed;
                out_palette.reserve(static_cast<std::size_t>(pl->value));
                for (int i = 0; i < pl->value; ++i) {
                    auto pv = terila::protocol::read_varint(data + pos, size - pos);
                    if (!pv) return false;
                    pos += pv->consumed;
                    out_palette.push_back(pv->value);
                }
            }

            const int bits = out_bits;
            const int entries_per_long = 64 / bits;
            const int long_count = (entry_count + entries_per_long - 1) / entries_per_long;
            const std::size_t bytes_needed = static_cast<std::size_t>(long_count) * 8;
            if (pos + bytes_needed > size) return false;

            out_states.resize(static_cast<std::size_t>(entry_count));
            const std::uint64_t mask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1);
            for (int i = 0; i < entry_count; ++i) {
                const int li = i / entries_per_long;
                const int off = (i % entries_per_long) * bits;
                std::uint64_t v = 0;
                for (int b = 0; b < 8; ++b)
                    v = (v << 8) | data[pos + static_cast<std::size_t>(li) * 8 + b];
                const std::uint64_t value = (v >> off) & mask;
                if (!is_direct && value >= out_palette.size()) return false;
                out_states[i] = static_cast<std::int32_t>(value);
            }

            pos += bytes_needed;
            return true;
        }

    }

    inline std::optional<Chunk> decode(const std::uint8_t* data, std::size_t size,
        std::int32_t section_count = 24) {
        if (data == nullptr || size < 8) return std::nullopt;

        std::size_t pos = 0;
        Chunk c;
        c.x = detail::read_i32_be(data + pos);      pos += 4;
        c.z = detail::read_i32_be(data + pos);      pos += 4;
        c.section_count = section_count;

        auto hm_count = terila::protocol::read_varint(data + pos, size - pos);
        if (!hm_count || hm_count->value < 0) return std::nullopt;
        pos += hm_count->consumed;
        c.heightmaps.reserve(static_cast<std::size_t>(hm_count->value));
        for (int i = 0; i < hm_count->value; ++i) {
            auto tid = terila::protocol::read_varint(data + pos, size - pos);
            if (!tid) return std::nullopt;
            pos += tid->consumed;
            auto lc = terila::protocol::read_varint(data + pos, size - pos);
            if (!lc || lc->value < 0) return std::nullopt;
            pos += lc->consumed;
            if (pos + static_cast<std::size_t>(lc->value) * 8 > size) return std::nullopt;

            Heightmap hm;
            hm.type_id = tid->value;
            hm.longs.resize(static_cast<std::size_t>(lc->value));
            for (int j = 0; j < lc->value; ++j) {
                hm.longs[j] = detail::read_i64_be(data + pos);
                pos += 8;
            }
            c.heightmaps.push_back(std::move(hm));
        }

        auto data_len = terila::protocol::read_varint(data + pos, size - pos);
        if (!data_len || data_len->value < 0) return std::nullopt;
        pos += data_len->consumed;
        if (pos + static_cast<std::size_t>(data_len->value) > size) return std::nullopt;

        const std::uint8_t* data_start = data + pos;
        const std::size_t   data_size = static_cast<std::size_t>(data_len->value);
        pos += data_size;

        // The blob is a plain array of section_count sections (no count prefix).
        std::size_t sub_pos = 0;
        const int nsec = section_count;

        c.sections.reserve(static_cast<std::size_t>(nsec));
        for (int s = 0; s < nsec; ++s) {
            if (sub_pos + 4 > data_size) {
                LOG_WARN(Chunk, "decode: only parsed {} of {} sections", s, nsec);
                break;
            }
            ChunkSection sec;
            sec.block_count = detail::read_u16_be(data_start + sub_pos);
            sec.fluid_count = detail::read_u16_be(data_start + sub_pos + 2);
            sub_pos += 4;

            if (!detail::decode_palette_container(data_start, data_size, sub_pos,
                4096, 9, sec.block_bits, sec.block_palette, sec.block_states)) {
                LOG_WARN(Chunk, "decode: block palette failed at section {}", s);
                return std::nullopt;
            }
            if (!detail::decode_palette_container(data_start, data_size, sub_pos,
                64, 4, sec.biome_bits, sec.biome_palette, sec.biome_states)) {
                LOG_WARN(Chunk, "decode: biome palette failed at section {}", s);
                return std::nullopt;
            }
            c.sections.push_back(std::move(sec));
        }

        if (pos < size) {
            auto be_count = terila::protocol::read_varint(data + pos, size - pos);
            if (be_count) {
                pos += be_count->consumed;
                if (be_count->value > 0) {
                    LOG_WARN(Chunk, "decode: block_entities count={} not supported", be_count->value);
                    return std::nullopt;
                }
            }
        }

        auto read_bitset = [&](std::vector<std::int64_t>& out) -> bool {
            auto cnt = terila::protocol::read_varint(data + pos, size - pos);
            if (!cnt || cnt->value < 0) return false;
            pos += cnt->consumed;
            const std::size_t nbytes = static_cast<std::size_t>(cnt->value);
            if (pos + nbytes > size) return false;
            out.assign((nbytes + 7) / 8, 0);
            for (std::size_t i = 0; i < nbytes; ++i)
                out[i / 8] |= static_cast<std::int64_t>(static_cast<std::uint64_t>(data[pos + i]) << ((i % 8) * 8));
            pos += nbytes;
            return true;
            };
        if (!read_bitset(c.light.sky_y_mask))         return std::nullopt;
        if (!read_bitset(c.light.block_y_mask))       return std::nullopt;
        if (!read_bitset(c.light.empty_sky_y_mask))   return std::nullopt;
        if (!read_bitset(c.light.empty_block_y_mask)) return std::nullopt;

        auto read_arrays = [&](std::vector<std::vector<std::uint8_t>>& out) -> bool {
            auto cnt = terila::protocol::read_varint(data + pos, size - pos);
            if (!cnt || cnt->value < 0) return false;
            pos += cnt->consumed;
            out.resize(static_cast<std::size_t>(cnt->value));
            for (int i = 0; i < cnt->value; ++i) {
                auto len = terila::protocol::read_varint(data + pos, size - pos);
                if (!len || len->value < 0) return false;
                pos += len->consumed;
                if (pos + static_cast<std::size_t>(len->value) > size) return false;
                out[i].assign(data + pos, data + pos + len->value);
                pos += static_cast<std::size_t>(len->value);
            }
            return true;
            };
        if (!read_arrays(c.light.sky_updates))   return std::nullopt;
        if (!read_arrays(c.light.block_updates)) return std::nullopt;

        return c;
    }

}