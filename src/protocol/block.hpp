#pragma once

#include "log/logger.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/primitives.hpp"
#include "protocol/varint.hpp"

#include <cstdint>
#include <vector>

namespace terila::protocol {

    // BlockPos is packed into one long: x (26 bits) | z (26 bits) | y (12 bits).
    inline std::int64_t pack_block_pos(std::int32_t x, std::int32_t y, std::int32_t z) {
        const auto ux = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0x3FFFFFFu);
        const auto uz = static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x3FFFFFFu);
        const auto uy = static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0xFFFu);
        return static_cast<std::int64_t>((ux << 38) | (uz << 12) | uy);
    }

    struct BlockPosValue {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
    };

    inline BlockPosValue unpack_block_pos(const std::uint8_t* data, std::size_t length) {
        BlockPosValue out;
        const auto raw = read_int64(data, length);
        if (!raw.has_value()) {
            return out;
        }
        const auto value = static_cast<std::uint64_t>(*raw);
        out.y = static_cast<std::int32_t>((static_cast<std::int64_t>(value << 52)) >> 52);
        out.z = static_cast<std::int32_t>((static_cast<std::int64_t>(value << 26)) >> 38);
        out.x = static_cast<std::int32_t>(static_cast<std::int64_t>(value) >> 38);
        return out;
    }

    inline bool write_block_update(std::vector<std::uint8_t>& output,
        std::int32_t x, std::int32_t y, std::int32_t z, std::int32_t block_state)
    {
        std::vector<std::uint8_t> payload;
        write_int64(payload, pack_block_pos(x, y, z));
        write_varint(payload, block_state);

        if (!write_packet(output, packet_ids().play_block_update, payload)) {
            LOG_WARN(Protocol, "write_block_update: failed at ({}, {}, {})", x, y, z);
            return false;
        }
        return true;
    }

    // The client predicts block changes; the ack confirms its sequence number so
    // the prediction is not rolled back.
    inline bool write_block_changed_ack(std::vector<std::uint8_t>& output, std::int32_t sequence) {
        std::vector<std::uint8_t> payload;
        write_varint(payload, sequence);

        if (!write_packet(output, packet_ids().play_block_changed_ack, payload)) {
            LOG_WARN(Protocol, "write_block_changed_ack: failed (sequence={})", sequence);
            return false;
        }
        return true;
    }

    inline bool write_section_blocks_update(std::vector<std::uint8_t>& output,
        std::int32_t section_x, std::int32_t section_y, std::int32_t section_z,
        const std::vector<std::pair<std::int16_t, std::int32_t>>& blocks)
    {
        std::vector<std::uint8_t> payload;
        const std::int64_t section_pos =
            (static_cast<std::int64_t>(section_x & 0x3FFFFF) << 42) |
            (static_cast<std::int64_t>(section_z & 0x3FFFFF) << 20) |
            static_cast<std::int64_t>(section_y & 0xFFFFF);
        write_int64(payload, section_pos);

        write_varint(payload, static_cast<std::int32_t>(blocks.size()));
        for (const auto& block : blocks) {
            write_int16(payload, block.first);
        }
        for (const auto& block : blocks) {
            write_varint(payload, block.second);
        }

        if (!write_packet(output, packet_ids().play_section_blocks_update, payload)) {
            LOG_WARN(Protocol, "write_section_blocks_update: failed ({} blocks)", blocks.size());
            return false;
        }
        return true;
    }

}
