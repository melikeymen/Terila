#pragma once

#include "log/logger.hpp"
#include "protocol/varint.hpp"
#include "protocol/string.hpp"
#include "protocol/packet.hpp"
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

        // playerId — 4-byte int (NOT VarInt)
        payload.push_back(static_cast<std::uint8_t>((entity_id >> 24) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((entity_id >> 16) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((entity_id >> 8) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>(entity_id & 0xFF));

        // hardcore
        payload.push_back(hardcore ? 1 : 0);

        // levels — 3 vanilla dimensions (client expects all)
        write_varint(payload, 3);
        write_string(payload, "minecraft:overworld");
        write_string(payload, "minecraft:the_nether");
        write_string(payload, "minecraft:the_end");

        // maxPlayers
        write_varint(payload, 20);

        // chunkRadius
        write_varint(payload, 10);

        // simulationDistance
        write_varint(payload, 10);

        // reducedDebugInfo
        payload.push_back(0);

        // showDeathScreen
        payload.push_back(1);

        // doLimitedCrafting
        payload.push_back(0);

        // commonPlayerSpawnInfo
        write_varint(payload, 0);              // dimensionType (registry index)
        write_string(payload, dimension);      // dimension name
        payload.insert(payload.end(), 8, 0);   // seed (long)
        payload.push_back(gamemode);           // gameType (byte)
        payload.push_back(0);                  // previousGameType (byte)
        payload.push_back(0);                  // isDebug
        payload.push_back(0);                  // isFlat
        payload.push_back(0);                  // hasDeathLocation
        write_varint(payload, 0);              // portalCooldown
        write_varint(payload, 63);             // seaLevel

        // onlineMode
        payload.push_back(0);

        // enforcesSecureChat
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

        auto write_double = [](std::vector<std::uint8_t>& out, double v) {
            std::uint64_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            for (int i = 7; i >= 0; --i)
                out.push_back(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF));
            };
        auto write_float = [](std::vector<std::uint8_t>& out, float v) {
            std::uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            for (int i = 3; i >= 0; --i)
                out.push_back(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF));
            };

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

}