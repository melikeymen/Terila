#pragma once

#include "log/logger.hpp"
#include "protocol/varint.hpp"
#include "protocol/string.hpp"
#include "protocol/packet.hpp"
#include <vector>
#include <cstdint>
#include <string>
#include <cstring>



namespace terila::protocol {

    inline bool write_login_play(std::vector<std::uint8_t>& output,
        std::int32_t entity_id, bool hardcore,
        std::uint8_t gamemode, const std::string& dimension) {

        // currently some data is static

        std::vector<std::uint8_t> payload;

        // Entity ID — unique per player, assigned by server (used in most play packets)
        write_varint(payload, entity_id);

        // Is Hardcore — if true, hearts show as hardcore texture
        payload.push_back(hardcore ? 1 : 0);

        // Dimension Names — array of all dimensions the client knows about
        // Count (VarInt): how many dimensions follow
        write_varint(payload, 1);
        // First (and only) dimension name
        write_string(payload, dimension);

        // Max Players — shown in the tab list (informational, not enforced here)
        write_varint(payload, 20);

        // View Distance — how many chunks the client renders around itself
        write_varint(payload, 10);

        // Simulation Distance — how far entities/ticks are simulated
        write_varint(payload, 10);

        // Reduced Debug Info — if true, F3 screen shows less info (anti-cheat)
        payload.push_back(0);

        // Enable Respawn Screen — if true, death shows "Respawn" button
        payload.push_back(1);

        // Do Limited Crafting — if true, crafting recipes are limited (1.20.2+)
        payload.push_back(0);

        // Dimension Type — ID into `minecraft:dimension_type` registry
        // 0 = overworld (registry index, not the dimension name)
        write_varint(payload, 0);

        // Dimension Name — used for respawn; must match a name in Dimension Names above
        write_string(payload, dimension);

        // Hashed Seed — 8 bytes (Long); hash of world seed for client-side features
        // (biome blending, structure locating). 0 is safe for now.
        payload.insert(payload.end(), 8, 0);

        // Game Mode — 0=survival, 1=creative, 2=adventure, 3=spectator
        payload.push_back(gamemode);

        // Previous Game Mode — 0xFF means -1 (no previous; first join)
        payload.push_back(0xFF);

        // Is Debug World — true for the flat debug world (all block states)
        payload.push_back(0);

        // Is Flat World — true if the world was generated flat
        payload.push_back(0);

        // Has Death Location — if false, no extra fields follow
        // (if true: dimension name + 8-byte position would follow)
        payload.push_back(0);

        // Portal Cooldown — ticks until player can use a portal again
        write_varint(payload, 0);

        // Sea Level — Y coordinate of sea level (63 overworld, -63 nether/end)
        // Used for ambient/fog rendering; added in 1.21.2
        write_varint(payload, 63);

        if (!write_packet(output, 0x2B, payload)) {
            LOG_WARN(Protocol, "write_login_play: failed to write packet");
            return false;
        }

        LOG_DEBUG(Protocol, "write_login_play: entity_id={} dimension={}", entity_id, dimension);
        return true;

    }

    // Set Center Chunk (0x52) — client keeps chunks around this point loaded
    inline bool write_set_center_chunk(std::vector<std::uint8_t>& output,
        std::int32_t chunk_x, std::int32_t chunk_z)
    {
        std::vector<std::uint8_t> payload;

        // Chunk X — 16-block units (chunk coordinate), not blocks
        write_varint(payload, chunk_x);
        // Chunk Z
        write_varint(payload, chunk_z);

        if (!write_packet(output, 0x52, payload)) {
            LOG_WARN(Protocol, "write_set_center_chunk: failed");
            return false;
        }
        LOG_DEBUG(Protocol, "set_center_chunk: ({}, {})", chunk_x, chunk_z);
        return true;
    }

    // Player Position (0x40) — absolute teleport
    inline bool write_player_position(std::vector<std::uint8_t>& output,
        double x, double y, double z,
        float yaw, float pitch, std::int32_t teleport_id)
    {
        std::vector<std::uint8_t> payload;

        // X, Y, Z — 8 bytes each (big-endian double)
        auto write_double = [](std::vector<std::uint8_t>& out, double v) {
            std::uint64_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            for (int i = 7; i >= 0; --i) {
                out.push_back(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF));
            }
            };

        // Yaw, Pitch — 4 bytes each (big-endian float)
        auto write_float = [](std::vector<std::uint8_t>& out, float v) {
            std::uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            for (int i = 3; i >= 0; --i) {
                out.push_back(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF));
            }
            };

        write_double(payload, x);
        write_double(payload, y);
        write_double(payload, z);
        write_float(payload, yaw);
        write_float(payload, pitch);

        // Flags — 0 = all absolute (bit per axis: 0x01=X, 0x02=Y, 0x04=Z, etc.)
        payload.push_back(0);

        // Teleport ID — client echoes in Confirm Teleport (0x00) to acknowledge
        write_varint(payload, teleport_id);

        if (!write_packet(output, 0x40, payload)) {
            LOG_WARN(Protocol, "write_player_position: failed");
            return false;
        }
        LOG_DEBUG(Protocol, "player_position: ({:.2f}, {:.2f}, {:.2f})", x, y, z);
        return true;
    }

}