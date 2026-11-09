#pragma once

#include <cstdint>

namespace terila::protocol {

    struct PacketIds {
        // Handshake
        std::int32_t handshake = 0x00;

        // Status
        std::int32_t status_request = 0x00;
        std::int32_t status_response = 0x00;
        std::int32_t ping = 0x01;

        // Login
        std::int32_t login_start = 0x00;
        std::int32_t login_success = 0x02;
        std::int32_t login_acknowledged = 0x03;

        // Configuration
        std::int32_t config_update_enabled_features = 0x0D;
        std::int32_t config_select_known_packs = 0x0F;
        std::int32_t config_registry_data = 0x07;
        std::int32_t config_update_tags = 0x0E;
        std::int32_t config_finish = 0x03;

        // Play
        std::int32_t play_login = 0x32;
        std::int32_t play_player_position = 0x49;
        std::int32_t play_game_event = 0x27;
        std::int32_t play_level_chunk = 0x2E;
    };

    inline PacketIds& packet_ids() {
        static PacketIds ids;
        return ids;
    }

}