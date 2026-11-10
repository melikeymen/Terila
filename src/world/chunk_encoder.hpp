#pragma once

#include "world/chunk.hpp"
#include "protocol/varint.hpp"
#include "protocol/primitives.hpp"
#include "log/logger.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace terila::world::encode {

    namespace detail {

        inline void encode_palette_container(std::vector<std::uint8_t>& out,
            std::uint8_t bits,
            const std::vector<std::int32_t>& palette,
            const std::vector<std::int32_t>& states,
            int entry_count, int min_bits_direct)
        {
            out.push_back(bits);

            if (bits == 0) {
                const std::int32_t v = palette.empty() ? 0 : palette[0];
                terila::protocol::write_varint(out, v);
                return;
            }

            const bool is_direct = bits >= min_bits_direct;
            if (!is_direct) {
                terila::protocol::write_varint(out, static_cast<std::int32_t>(palette.size()));
                for (auto v : palette) terila::protocol::write_varint(out, v);
            }

            const int entries_per_long = 64 / bits;
            const int long_count = (entry_count + entries_per_long - 1) / entries_per_long;
            std::vector<std::uint64_t> longs(static_cast<std::size_t>(long_count), 0);
            const std::uint64_t mask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1);
            for (int i = 0; i < entry_count; ++i) {
                std::uint64_t v = (i < static_cast<int>(states.size()))
                    ? static_cast<std::uint64_t>(states[i]) : 0ull;
                v &= mask;
                const int li = i / entries_per_long;
                const int off = (i % entries_per_long) * bits;
                longs[li] |= (v << off);
            }
            for (auto l : longs) {
                for (int b = 7; b >= 0; --b)
                    out.push_back(static_cast<std::uint8_t>((l >> (b * 8)) & 0xFF));
            }
        }

    }

    inline std::vector<std::uint8_t> encode(const Chunk& c) {
        std::vector<std::uint8_t> out;

        terila::protocol::write_int32(out, c.x);
        terila::protocol::write_int32(out, c.z);

        terila::protocol::write_varint(out, static_cast<std::int32_t>(c.heightmaps.size()));
        for (const auto& hm : c.heightmaps) {
            terila::protocol::write_varint(out, hm.type_id);
            terila::protocol::write_varint(out, static_cast<std::int32_t>(hm.longs.size()));
            for (auto v : hm.longs) terila::protocol::write_int64(out, v);
        }

        std::vector<std::uint8_t> data_buf;
        // The data blob holds exactly section_count sections; neither the count nor the
        // palette long arrays are length-prefixed on the wire.
        for (const auto& sec : c.sections) {
            terila::protocol::write_uint16(data_buf, sec.block_count);
            terila::protocol::write_uint16(data_buf, sec.fluid_count);
            detail::encode_palette_container(data_buf, sec.block_bits,
                sec.block_palette, sec.block_states, 4096, 9);
            detail::encode_palette_container(data_buf, sec.biome_bits,
                sec.biome_palette, sec.biome_states, 64, 4);
        }
        terila::protocol::write_varint(out, static_cast<std::int32_t>(data_buf.size()));
        out.insert(out.end(), data_buf.begin(), data_buf.end());

        terila::protocol::write_varint(out, 0);  // block_entities

        // 26.3 sends light masks as BitSet.toByteArray(): VarInt byte length, then the bytes
        // with trailing zero bytes trimmed.
        auto write_bitset = [&](const std::vector<std::int64_t>& bs) {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(bs.size() * 8);
            for (auto v : bs) {
                const auto u = static_cast<std::uint64_t>(v);
                for (int b = 0; b < 8; ++b)
                    bytes.push_back(static_cast<std::uint8_t>((u >> (b * 8)) & 0xFF));
            }
            while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
            terila::protocol::write_varint(out, static_cast<std::int32_t>(bytes.size()));
            out.insert(out.end(), bytes.begin(), bytes.end());
            };
        write_bitset(c.light.sky_y_mask);
        write_bitset(c.light.block_y_mask);
        write_bitset(c.light.empty_sky_y_mask);
        write_bitset(c.light.empty_block_y_mask);

        auto write_arrays = [&](const std::vector<std::vector<std::uint8_t>>& arrays) {
            terila::protocol::write_varint(out, static_cast<std::int32_t>(arrays.size()));
            for (const auto& a : arrays) {
                terila::protocol::write_varint(out, static_cast<std::int32_t>(a.size()));
                out.insert(out.end(), a.begin(), a.end());
            }
            };
        write_arrays(c.light.sky_updates);
        write_arrays(c.light.block_updates);

        return out;
    }

}