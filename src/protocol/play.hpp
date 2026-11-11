#pragma once

#include "log/logger.hpp"
#include "protocol/varint.hpp"
#include "protocol/string.hpp"
#include "protocol/packet.hpp"
#include "protocol/primitives.hpp"
#include "protocol/packet_ids.hpp"
#include <vector>
#include <cstdint>
#include <string>
#include <cstring>

namespace terila::protocol {

    inline bool write_login_play(std::vector<std::uint8_t>& output,
        std::int32_t entity_id, bool hardcore,
        std::uint8_t gamemode, const std::string& dimension)
    {
        std::vector<std::uint8_t> payload;

        payload.push_back(static_cast<std::uint8_t>((entity_id >> 24) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((entity_id >> 16) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((entity_id >> 8) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>(entity_id & 0xFF));

        payload.push_back(hardcore ? 1 : 0);

        write_varint(payload, 3);
        write_string(payload, "minecraft:overworld");
        write_string(payload, "minecraft:the_nether");
        write_string(payload, "minecraft:the_end");

        write_varint(payload, 20);
        write_varint(payload, 10);
        write_varint(payload, 10);
        payload.push_back(0);
        payload.push_back(1);
        payload.push_back(0);

        write_varint(payload, 0);
        write_string(payload, dimension);
        payload.insert(payload.end(), 8, 0);
        payload.push_back(gamemode);
        payload.push_back(0);
        payload.push_back(0);
        payload.push_back(0);
        payload.push_back(0);
        write_varint(payload, 0);
        write_varint(payload, 63);

        payload.push_back(0);
        payload.push_back(0);

        if (!write_packet(output, packet_ids().play_login, payload)) {
            LOG_WARN(Protocol, "write_login_play: failed to write packet");
            return false;
        }
        LOG_DEBUG(Protocol, "write_login_play: entity_id={} dimension={}", entity_id, dimension);
        return true;
    }

    inline bool write_player_position(std::vector<std::uint8_t>& output,
        std::int32_t teleport_id,
        double x, double y, double z,
        double dx, double dy, double dz,
        float yaw, float pitch)
    {
        std::vector<std::uint8_t> payload;

        write_varint(payload, teleport_id);
        write_double(payload, x);
        write_double(payload, y);
        write_double(payload, z);
        write_double(payload, dx);
        write_double(payload, dy);
        write_double(payload, dz);
        write_float(payload, yaw);
        write_float(payload, pitch);
        payload.push_back(0);
        payload.push_back(0);
        payload.push_back(0);
        payload.push_back(0);

        if (!write_packet(output, packet_ids().play_player_position, payload)) {
            LOG_WARN(Protocol, "write_player_position: failed");
            return false;
        }
        LOG_DEBUG(Protocol, "player_position: ({:.2f}, {:.2f}, {:.2f})", x, y, z);
        return true;
    }

    inline bool write_set_center_chunk(std::vector<std::uint8_t>& output,
        std::int32_t cx, std::int32_t cz)
    {
        std::vector<std::uint8_t> payload;
        write_varint(payload, cx);
        write_varint(payload, cz);
        if (!write_packet(output, packet_ids().play_set_center_chunk, payload)) {
            LOG_WARN(Protocol, "write_set_center_chunk: failed");
            return false;
        }
        LOG_DEBUG(Protocol, "set_center_chunk: ({}, {})", cx, cz);
        return true;
    }

    inline bool write_game_event(std::vector<std::uint8_t>& output,
        std::uint8_t event_id, float value)
    {
        std::vector<std::uint8_t> payload;
        payload.push_back(event_id);
        write_float(payload, value);
        if (!write_packet(output, packet_ids().play_game_event, payload)) {
            LOG_WARN(Protocol, "write_game_event: failed");
            return false;
        }
        return true;
    }

    inline bool write_chunk_batch_start(std::vector<std::uint8_t>& output)
    {
        if (!write_packet(output, packet_ids().play_chunk_batch_start, {})) {
            LOG_WARN(Protocol, "write_chunk_batch_start: failed");
            return false;
        }
        return true;
    }

    inline bool write_chunk_batch_finished(std::vector<std::uint8_t>& output, std::int32_t batch_size)
    {
        std::vector<std::uint8_t> payload;
        write_varint(payload, batch_size);
        if (!write_packet(output, packet_ids().play_chunk_batch_finished, payload)) {
            LOG_WARN(Protocol, "write_chunk_batch_finished: failed");
            return false;
        }
        return true;
    }

    inline bool write_chunk_data(std::vector<std::uint8_t>& output,
        const std::vector<std::uint8_t>& encoded_payload)
    {
        if (!write_packet(output, packet_ids().play_level_chunk, encoded_payload)) {
            LOG_WARN(Protocol, "write_chunk_data: failed ({} bytes)", encoded_payload.size());
            return false;
        }
        LOG_TRACE(Protocol, "chunk_data: {} bytes", encoded_payload.size());
        return true;
    }

    inline bool write_keep_alive(std::vector<std::uint8_t>& output, std::int64_t id)
    {
        std::vector<std::uint8_t> payload;
        write_int64(payload, id);
        if (!write_packet(output, packet_ids().play_keep_alive, payload)) {
            LOG_WARN(Protocol, "write_keep_alive: failed (id={})", id);
            return false;
        }
        return true;
    }

}