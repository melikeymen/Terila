#pragma once

#include "log/logger.hpp"
#include "protocol/block.hpp"
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

    // `dimension_type` is the index of the dimension type in the registry the client was
    // handed during configuration, which is what tells it the height, the skylight and the
    // coordinate scale of the world it is about to receive.
    inline bool write_login_play(std::vector<std::uint8_t>& output,
        std::int32_t entity_id, bool hardcore,
        std::uint8_t gamemode, const std::string& dimension,
        int max_players, int view_distance, int simulation_distance,
        std::int32_t dimension_type = 0)
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

        write_varint(payload, max_players);
        write_varint(payload, view_distance);
        write_varint(payload, simulation_distance);
        payload.push_back(0);
        payload.push_back(1);
        payload.push_back(0);

        write_varint(payload, dimension_type);
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

    // The world's difficulty and whether it is locked: the client shows it on the debug
    // screen and uses it to decide whether hardcore behaviour applies.
    inline bool write_change_difficulty(std::vector<std::uint8_t>& output,
        std::uint8_t difficulty, bool locked)
    {
        std::vector<std::uint8_t> payload;
        payload.push_back(difficulty);
        payload.push_back(locked ? 1 : 0);

        if (!write_packet(output, packet_ids().play_change_difficulty, payload)) {
            LOG_WARN(Protocol, "write_change_difficulty: failed to write packet");
            return false;
        }
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

    // Bit 0 invulnerable, 1 flying, 2 may fly, 3 instabuild. The two floats are the
    // flying speed and the field of view modifier.
    inline bool write_player_abilities(std::vector<std::uint8_t>& output, std::uint8_t flags,
        float flying_speed = 0.05f, float fov_modifier = 0.1f)
    {
        std::vector<std::uint8_t> payload;
        payload.push_back(flags);
        write_float(payload, flying_speed);
        write_float(payload, fov_modifier);
        if (!write_packet(output, packet_ids().play_player_abilities, payload)) {
            LOG_WARN(Protocol, "write_player_abilities: failed");
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

    // --- serverbound play packets ------------------------------------------------

    // set_creative_mode_slot is the slot number followed by the stack itself: a count
    // varint and, only when the count is not zero, the item id and its components. There
    // is no button byte between them, so reading one byte late turns every pick into an
    // empty slot.
    struct CreativeSlotUpdate {
        std::int32_t slot = 0;
        std::int32_t count = 0;
        std::int32_t item_id = 0;
    };

    inline std::optional<CreativeSlotUpdate> read_creative_slot(
        const std::uint8_t* data, std::size_t length) {
        if (length < 2) {
            return std::nullopt;
        }

        CreativeSlotUpdate out;
        out.slot = static_cast<std::int32_t>((data[0] << 8) | data[1]);
        std::size_t offset = 2;

        const auto count = read_varint(data + offset, length - offset);
        if (!count) {
            return std::nullopt;
        }
        offset += count->consumed;
        if (count->value <= 0) {
            // An empty slot stops here; an item id would be meaningless.
            return out;
        }

        const auto item = read_varint(data + offset, length - offset);
        if (!item) {
            return std::nullopt;
        }

        out.item_id = item->value;
        out.count = count->value;
        return out;
    }

    struct UseItemOn {
        BlockPosValue pos{};
        std::int32_t face = 0;
        std::int32_t sequence = 0;
    };

    // hand (varint), block position (long), face (varint), cursor x/y/z (three floats),
    // inside-block flag (bool), sequence (varint).
    inline std::optional<UseItemOn> read_use_item_on(const std::uint8_t* data, std::size_t length) {
        std::size_t offset = 0;

        const auto hand = read_varint(data, length);
        if (!hand) {
            return std::nullopt;
        }
        offset += hand->consumed;
        if (length < offset + 8 + 1) {
            return std::nullopt;
        }

        UseItemOn out;
        out.pos = unpack_block_pos(data + offset, length - offset);
        offset += 8;

        const auto face = read_varint(data + offset, length - offset);
        if (!face) {
            return std::nullopt;
        }
        out.face = face->value;
        offset += face->consumed;

        // The cursor position and the inside-block flag sit between the face and the
        // sequence; getting this length wrong only costs the ack its sequence number.
        offset += 12 + 1;
        if (length >= offset) {
            if (const auto sequence = read_varint(data + offset, length - offset)) {
                out.sequence = sequence->value;
            }
        }

        return out;
    }

}