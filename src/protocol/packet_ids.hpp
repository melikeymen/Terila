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
        std::int32_t status_pong = 0x01;

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

        // Serverbound configuration packets carry their own numbering; matching a
        // client packet against a clientbound id only works by coincidence.
        std::int32_t config_client_information_serverbound = 0x00;
        std::int32_t config_select_known_packs_serverbound = 0x07;
        std::int32_t config_finish_serverbound = 0x03;

        // Play — must be resolved from ids.json at startup
        std::int32_t play_login = -1;
        std::int32_t play_player_position = -1;
        std::int32_t play_game_event = -1;
        std::int32_t play_set_center_chunk = -1;
        std::int32_t play_level_chunk = -1;
        std::int32_t play_chunk_batch_start = -1;
        std::int32_t play_chunk_batch_finished = -1;
        std::int32_t play_keep_alive = -1;
        std::int32_t play_keep_alive_serverbound = -1;
        std::int32_t play_move_player_pos = -1;
        std::int32_t play_move_player_pos_rot = -1;
        std::int32_t play_attack = -1;

        // Play — entity tracking
        std::int32_t play_add_entity = -1;
        std::int32_t play_remove_entities = -1;
        std::int32_t play_entity_position_sync = -1;
        std::int32_t play_move_entity_pos_rot = -1;
        std::int32_t play_move_entity_pos = -1;
        std::int32_t play_rotate_head = -1;
        std::int32_t play_entity_event = -1;
        std::int32_t play_hurt_animation = -1;
    };

    inline PacketIds& packet_ids() {
        static PacketIds ids;
        return ids;
    }

}