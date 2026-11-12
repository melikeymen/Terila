#pragma once

#include "log/logger.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/primitives.hpp"
#include "protocol/varint.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace terila::protocol {

    // Rotations travel as one byte: 256 steps over 360 degrees.
    inline std::uint8_t angle_to_byte(float degrees) {
        return static_cast<std::uint8_t>(static_cast<std::int8_t>(degrees * 256.0f / 360.0f));
    }

    // Field order from input/packets/structures.json:
    //   id, uuid, type, x, y, z, movement, xRot, yRot, yHeadRot, data
    inline bool write_add_entity(std::vector<std::uint8_t>& output,
        std::int32_t entity_id, const std::array<std::uint8_t, 16>& uuid,
        std::int32_t type_id, double x, double y, double z,
        float yaw, float pitch, float head_yaw,
        std::int16_t velocity_x = 0, std::int16_t velocity_y = 0, std::int16_t velocity_z = 0)
    {
        std::vector<std::uint8_t> payload;
        write_varint(payload, entity_id);
        payload.insert(payload.end(), uuid.begin(), uuid.end());
        write_varint(payload, type_id);
        write_double(payload, x);
        write_double(payload, y);
        write_double(payload, z);
        // Movement is a low precision Vec3: a zero vector collapses to a single
        // 0x00 byte, otherwise three 1/4096 shorts follow.
        if (velocity_x == 0 && velocity_y == 0 && velocity_z == 0) {
            payload.push_back(0);
        }
        else {
            write_int16(payload, velocity_x);
            write_int16(payload, velocity_y);
            write_int16(payload, velocity_z);
        }
        payload.push_back(angle_to_byte(pitch));
        payload.push_back(angle_to_byte(yaw));
        payload.push_back(angle_to_byte(head_yaw));
        write_varint(payload, 0);   // no entity specific data

        if (!write_packet(output, packet_ids().play_add_entity, payload)) {
            LOG_WARN(Protocol, "write_add_entity: failed (id={})", entity_id);
            return false;
        }
        return true;
    }

    inline bool write_remove_entities(std::vector<std::uint8_t>& output,
        const std::vector<std::int32_t>& entity_ids)
    {
        std::vector<std::uint8_t> payload;
        write_varint(payload, static_cast<std::int32_t>(entity_ids.size()));
        for (auto id : entity_ids) {
            write_varint(payload, id);
        }

        if (!write_packet(output, packet_ids().play_remove_entities, payload)) {
            LOG_WARN(Protocol, "write_remove_entities: failed ({} ids)", entity_ids.size());
            return false;
        }
        return true;
    }

    inline bool write_remove_entity(std::vector<std::uint8_t>& output, std::int32_t entity_id) {
        return write_remove_entities(output, std::vector<std::int32_t>{ entity_id });
    }

    // VecDelta: a varint properties field (bit 0 onGround, upper bits the step
    // count) then the movement. A zero step count selects the linear variant,
    // which is three 1/4096 delta shorts and nothing else.
    inline bool write_move_entity_pos_rot(std::vector<std::uint8_t>& output,
        std::int32_t entity_id, std::int16_t dx, std::int16_t dy, std::int16_t dz,
        float yaw, float pitch, bool on_ground)
    {
        std::vector<std::uint8_t> payload;
        write_varint(payload, entity_id);
        write_varint(payload, on_ground ? 1 : 0);
        write_int16(payload, dx);
        write_int16(payload, dy);
        write_int16(payload, dz);
        payload.push_back(angle_to_byte(yaw));
        payload.push_back(angle_to_byte(pitch));

        if (!write_packet(output, packet_ids().play_move_entity_pos_rot, payload)) {
            LOG_WARN(Protocol, "write_move_entity_pos_rot: failed (id={})", entity_id);
            return false;
        }
        return true;
    }

    // Same linear VecDelta as above, without the rotation bytes.
    inline bool write_move_entity_pos(std::vector<std::uint8_t>& output,
        std::int32_t entity_id, std::int16_t dx, std::int16_t dy, std::int16_t dz,
        bool on_ground)
    {
        std::vector<std::uint8_t> payload;
        write_varint(payload, entity_id);
        write_varint(payload, on_ground ? 1 : 0);
        write_int16(payload, dx);
        write_int16(payload, dy);
        write_int16(payload, dz);

        if (!write_packet(output, packet_ids().play_move_entity_pos, payload)) {
            LOG_WARN(Protocol, "write_move_entity_pos: failed (id={})", entity_id);
            return false;
        }
        return true;
    }

    // Entity event carries a plain int32 entity id. Event ids: 2 = hurt, 3 = death.
    inline bool write_entity_event(std::vector<std::uint8_t>& output,
        std::int32_t entity_id, std::uint8_t event_id)
    {
        std::vector<std::uint8_t> payload;
        write_int32(payload, entity_id);
        payload.push_back(event_id);

        if (!write_packet(output, packet_ids().play_entity_event, payload)) {
            LOG_WARN(Protocol, "write_entity_event: failed (id={})", entity_id);
            return false;
        }
        return true;
    }

    // Unlike entity event, hurt animation addresses the entity with a varint.
    inline bool write_hurt_animation(std::vector<std::uint8_t>& output,
        std::int32_t entity_id, float yaw)
    {
        std::vector<std::uint8_t> payload;
        write_varint(payload, entity_id);
        write_float(payload, yaw);

        if (!write_packet(output, packet_ids().play_hurt_animation, payload)) {
            LOG_WARN(Protocol, "write_hurt_animation: failed (id={})", entity_id);
            return false;
        }
        return true;
    }

    inline bool write_rotate_head(std::vector<std::uint8_t>& output,
        std::int32_t entity_id, float head_yaw)
    {
        std::vector<std::uint8_t> payload;
        write_varint(payload, entity_id);
        payload.push_back(angle_to_byte(head_yaw));

        if (!write_packet(output, packet_ids().play_rotate_head, payload)) {
            LOG_WARN(Protocol, "write_rotate_head: failed (id={})", entity_id);
            return false;
        }
        return true;
    }

}
