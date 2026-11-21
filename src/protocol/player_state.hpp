#pragma once

// The packets that carry a player's own state: health, the world clock, death and respawn.
// Field layouts follow ClientboundSetHealthPacket, ClientboundSetTimePacket,
// ClientboundPlayerCombatKillPacket, ClientboundRespawnPacket and ServerboundClientCommandPacket
// of the decompiled server.

#include "log/logger.hpp"
#include "nbt/nbt_text.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/primitives.hpp"
#include "protocol/string.hpp"
#include "protocol/varint.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terila::protocol {

    // LEB128 for a 64 bit value (ByteBufCodecs.VAR_LONG); the clock state is the only place
    // 26.3 uses one.
    inline void write_var_long(std::vector<std::uint8_t>& output, std::int64_t value) {
        auto bits = static_cast<std::uint64_t>(value);
        while (true) {
            if ((bits & ~0x7Full) == 0) {
                output.push_back(static_cast<std::uint8_t>(bits));
                return;
            }
            output.push_back(static_cast<std::uint8_t>((bits & 0x7Full) | 0x80ull));
            bits >>= 7;
        }
    }

    // health (float), food (varint), saturation (float).
    inline bool write_set_health(std::vector<std::uint8_t>& output, float health, int food,
        float saturation) {
        std::vector<std::uint8_t> payload;
        write_float(payload, health);
        write_varint(payload, food);
        write_float(payload, saturation);
        if (!write_packet(output, packet_ids().play_set_health, payload)) {
            LOG_WARN(Protocol, "write_set_health: failed");
            return false;
        }
        return true;
    }

    // game time (long), then the clock map: one entry for the overworld clock (registry id
    // 0), whose state is var-long total ticks, float partial tick, float rate.
    inline bool write_set_time(std::vector<std::uint8_t>& output, std::int64_t game_time,
        std::int64_t time_of_day) {
        std::vector<std::uint8_t> payload;
        write_int64(payload, game_time);
        write_varint(payload, 1);
        write_varint(payload, 0);            // minecraft:overworld world clock
        write_var_long(payload, time_of_day);
        write_float(payload, 0.0f);          // partial tick
        write_float(payload, 1.0f);          // rate: the clock keeps running
        if (!write_packet(output, packet_ids().play_set_time, payload)) {
            LOG_WARN(Protocol, "write_set_time: failed");
            return false;
        }
        return true;
    }

    // The death screen: the player id and the component that is shown as the cause.
    inline bool write_player_combat_kill(std::vector<std::uint8_t>& output,
        std::int32_t player_id, std::string_view message) {
        std::vector<std::uint8_t> payload;
        write_varint(payload, player_id);
        const auto component = terila::nbt::text_component_nbt(std::string(message));
        payload.insert(payload.end(), component.begin(), component.end());
        if (!write_packet(output, packet_ids().play_player_combat_kill, payload)) {
            LOG_WARN(Protocol, "write_player_combat_kill: failed");
            return false;
        }
        return true;
    }

    // CommonPlayerSpawnInfo (dimension type, dimension, seed, gamemode, previous gamemode,
    // debug, flat, last death, portal cooldown, sea level) followed by dataToKeep. The bytes
    // mirror write_login_play, which the client already accepts for the first spawn.
    inline bool write_respawn(std::vector<std::uint8_t>& output, const std::string& dimension,
        std::int64_t seed, std::uint8_t gamemode, int sea_level,
        std::int32_t dimension_type = 0) {
        std::vector<std::uint8_t> payload;
        write_varint(payload, dimension_type);
        write_string(payload, dimension);
        write_int64(payload, seed);
        payload.push_back(gamemode);
        payload.push_back(0);                // previous gamemode: absent
        payload.push_back(0);                // isDebug
        payload.push_back(0);                // isFlat
        payload.push_back(0);                // last death location: absent
        write_varint(payload, 0);            // portal cooldown
        write_varint(payload, sea_level);
        payload.push_back(0);                // dataToKeep: nothing

        if (!write_packet(output, packet_ids().play_respawn, payload)) {
            LOG_WARN(Protocol, "write_respawn: failed");
            return false;
        }
        return true;
    }

    // ServerboundClientCommandPacket.Action: 0 is "perform respawn", 1 "request stats".
    inline std::optional<std::int32_t> read_client_command(const std::vector<std::uint8_t>& payload) {
        const auto action = read_varint(payload.data(), payload.size());
        if (!action) {
            return std::nullopt;
        }
        return action->value;
    }

    // ServerboundInteractPacket: entity id, hand, the click position (three floats) and the
    // secondary-action flag.
    struct Interact {
        std::int32_t entity_id = 0;
        std::int32_t hand = 0;
        bool secondary = false;
    };

    inline std::optional<Interact> read_interact(const std::vector<std::uint8_t>& payload) {
        std::size_t offset = 0;
        const auto entity = read_varint(payload.data(), payload.size());
        if (!entity) {
            return std::nullopt;
        }
        offset += entity->consumed;

        const auto hand = read_varint(payload.data() + offset, payload.size() - offset);
        if (!hand) {
            return std::nullopt;
        }
        offset += hand->consumed;
        offset += 12;   // the click position: three floats

        Interact out;
        out.entity_id = entity->value;
        out.hand = hand->value;
        out.secondary = offset < payload.size() && payload[offset] != 0;
        return out;
    }

} // namespace terila::protocol
