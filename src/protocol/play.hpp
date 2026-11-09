#pragma once

#include "log/logger.hpp"
#include "protocol/varint.hpp"
#include "protocol/string.hpp"
#include "protocol/packet.hpp"
#include <vector>
#include <cstdint>
#include <string>


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

}