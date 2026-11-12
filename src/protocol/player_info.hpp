#pragma once

#include "log/logger.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/primitives.hpp"
#include "protocol/string.hpp"
#include "protocol/varint.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace terila::protocol {

    // ClientboundPlayerInfoUpdatePacket carries an EnumSet<Action> as a varint bit
    // field over the enum ordinals:
    //   0 ADD_PLAYER, 1 INITIALIZE_CHAT, 2 UPDATE_GAME_MODE, 3 UPDATE_LISTED,
    //   4 UPDATE_LATENCY, 5 UPDATE_DISPLAY_NAME, 6 UPDATE_LIST_ORDER, 7 UPDATE_HAT
    // Each entry then carries the selected actions in that same order, so the
    // reader walks the set, not the entry fields.
    inline constexpr std::int32_t kPlayerInfoAddBits = 0x1D;   // 0,2,3,4

    struct PlayerListEntry {
        std::array<std::uint8_t, 16> uuid{};
        std::string name;
        std::int32_t gamemode = 0;
        std::int32_t latency = 0;
        bool listed = true;
    };

    inline void write_player_info_entry(std::vector<std::uint8_t>& payload,
        const PlayerListEntry& entry)
    {
        payload.insert(payload.end(), entry.uuid.begin(), entry.uuid.end());

        // ADD_PLAYER: name, then the profile property list.
        write_string(payload, entry.name);
        write_varint(payload, 0);

        // UPDATE_GAME_MODE, UPDATE_LISTED, UPDATE_LATENCY, in ordinal order.
        write_varint(payload, entry.gamemode);
        payload.push_back(entry.listed ? 1 : 0);
        write_varint(payload, entry.latency);
    }

    inline bool write_player_info_add(std::vector<std::uint8_t>& output,
        const std::vector<PlayerListEntry>& entries)
    {
        if (entries.empty()) {
            return true;
        }

        std::vector<std::uint8_t> payload;
        write_varint(payload, kPlayerInfoAddBits);
        write_varint(payload, static_cast<std::int32_t>(entries.size()));
        for (const auto& entry : entries) {
            write_player_info_entry(payload, entry);
        }

        if (!write_packet(output, packet_ids().play_player_info_update, payload)) {
            LOG_WARN(Protocol, "write_player_info_add: failed ({} entries)", entries.size());
            return false;
        }
        return true;
    }

    // The remove packet lists raw UUIDs; there is no action set in it.
    inline bool write_player_info_remove(std::vector<std::uint8_t>& output,
        const std::vector<std::array<std::uint8_t, 16>>& uuids)
    {
        if (uuids.empty()) {
            return true;
        }

        std::vector<std::uint8_t> payload;
        write_varint(payload, static_cast<std::int32_t>(uuids.size()));
        for (const auto& uuid : uuids) {
            payload.insert(payload.end(), uuid.begin(), uuid.end());
        }

        if (!write_packet(output, packet_ids().play_player_info_remove, payload)) {
            LOG_WARN(Protocol, "write_player_info_remove: failed ({} uuids)", uuids.size());
            return false;
        }
        return true;
    }

}
