#pragma once

#include "protocol/packet_ids.hpp"
#include "log/logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

namespace terila::protocol {

    inline bool load_packet_ids(const std::string& path) {
        std::ifstream f(path);
        if (!f) {
            LOG_WARN(Protocol, "packet ids not found: {}", path);
            return false;
        }

        auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.contains("states")) {
            LOG_WARN(Protocol, "invalid packet ids json: {}", path);
            return false;
        }

        auto& ids = packet_ids();

        auto find = [&](const std::string& state, const std::string& direction,
            const std::string& name) -> std::int32_t {
                if (!j["states"].contains(state) || !j["states"][state].contains(direction))
                    return -1;
                for (const auto& pkt : j["states"][state][direction]) {
                    if (pkt.value("name", "") == name)
                        return pkt.value("protocol_id", -1);
                }
                return -1;
            };

        auto find_any = [&](std::initializer_list<const char*> names) -> std::int32_t {
            for (auto name : names) {
                auto v = find("play", "clientbound", name);
                if (v >= 0) return v;
            }
            return -1;
            };

        auto set = [&](std::int32_t& target, std::int32_t value, const char* label) {
            if (value >= 0) {
                target = value;
            }
            else {
                LOG_WARN(Protocol, "packet id missing: {}", label);
            }
            };

        // Handshake
        set(ids.handshake, find("handshake", "serverbound", "minecraft:intention"), "handshake");

        // Status
        set(ids.status_request, find("status", "serverbound", "minecraft:status_request"), "status_request");
        set(ids.status_response, find("status", "clientbound", "minecraft:status_response"), "status_response");
        set(ids.ping, find("status", "serverbound", "minecraft:ping_request"), "ping");
        set(ids.status_pong, find("status", "clientbound", "minecraft:pong_response"), "status_pong");

        // Login
        set(ids.login_start, find("login", "serverbound", "minecraft:hello"), "login_start");
        set(ids.login_success, find("login", "clientbound", "minecraft:login_finished"), "login_success");
        set(ids.login_acknowledged, find("login", "serverbound", "minecraft:login_acknowledged"), "login_acknowledged");

        // Configuration
        set(ids.config_update_enabled_features, find("configuration", "clientbound", "minecraft:update_enabled_features"), "config_update_enabled_features");
        set(ids.config_select_known_packs, find("configuration", "clientbound", "minecraft:select_known_packs"), "config_select_known_packs");
        set(ids.config_registry_data, find("configuration", "clientbound", "minecraft:registry_data"), "config_registry_data");
        set(ids.config_update_tags, find("configuration", "clientbound", "minecraft:update_tags"), "config_update_tags");
        set(ids.config_finish, find("configuration", "clientbound", "minecraft:finish_configuration"), "config_finish");
        set(ids.config_client_information_serverbound, find("configuration", "serverbound", "minecraft:client_information"), "config_client_information_serverbound");
        set(ids.config_select_known_packs_serverbound, find("configuration", "serverbound", "minecraft:select_known_packs"), "config_select_known_packs_serverbound");
        set(ids.config_finish_serverbound, find("configuration", "serverbound", "minecraft:finish_configuration"), "config_finish_serverbound");

        // Play — set_chunk_cache_center is 26.3's real name for what we call "set_center_chunk"
        set(ids.play_login, find("play", "clientbound", "minecraft:login"), "play_login");
        set(ids.play_player_position, find("play", "clientbound", "minecraft:player_position"), "play_player_position");
        set(ids.play_game_event, find("play", "clientbound", "minecraft:game_event"), "play_game_event");
        set(ids.play_set_center_chunk, find_any({ "minecraft:set_chunk_cache_center",
                                                  "minecraft:set_center_chunk" }), "play_set_center_chunk");
        set(ids.play_level_chunk, find("play", "clientbound", "minecraft:level_chunk_with_light"), "play_level_chunk");
        set(ids.play_chunk_batch_start, find("play", "clientbound", "minecraft:chunk_batch_start"), "play_chunk_batch_start");
        set(ids.play_chunk_batch_finished, find("play", "clientbound", "minecraft:chunk_batch_finished"), "play_chunk_batch_finished");
        set(ids.play_keep_alive, find("play", "clientbound", "minecraft:keep_alive"), "play_keep_alive");
        set(ids.play_keep_alive_serverbound, find("play", "serverbound", "minecraft:keep_alive"), "play_keep_alive_serverbound");
        set(ids.play_move_player_pos, find("play", "serverbound", "minecraft:move_player_pos"), "play_move_player_pos");
        set(ids.play_move_player_pos_rot, find("play", "serverbound", "minecraft:move_player_pos_rot"), "play_move_player_pos_rot");
        set(ids.play_attack, find("play", "serverbound", "minecraft:attack"), "play_attack");
        set(ids.play_player_action, find("play", "serverbound", "minecraft:player_action"), "play_player_action");
        set(ids.play_use_item_on, find("play", "serverbound", "minecraft:use_item_on"), "play_use_item_on");
        set(ids.play_set_creative_mode_slot, find("play", "serverbound", "minecraft:set_creative_mode_slot"), "play_set_creative_mode_slot");
        set(ids.play_set_carried_item, find("play", "serverbound", "minecraft:set_carried_item"), "play_set_carried_item");
        set(ids.play_pick_item_from_block, find("play", "serverbound", "minecraft:pick_item_from_block"), "play_pick_item_from_block");
        set(ids.play_change_game_mode, find("play", "serverbound", "minecraft:change_game_mode"), "play_change_game_mode");
        set(ids.play_container_click, find("play", "serverbound", "minecraft:container_click"), "play_container_click");
        set(ids.play_container_close_serverbound, find("play", "serverbound", "minecraft:container_close"), "play_container_close_serverbound");

        // Inventory and dropped items
        set(ids.play_container_set_slot, find("play", "clientbound", "minecraft:container_set_slot"), "play_container_set_slot");
        set(ids.play_player_abilities, find("play", "clientbound", "minecraft:player_abilities"), "play_player_abilities");
        set(ids.play_open_screen, find("play", "clientbound", "minecraft:open_screen"), "play_open_screen");
        set(ids.play_container_set_content, find("play", "clientbound", "minecraft:container_set_content"), "play_container_set_content");
        set(ids.play_container_set_data, find("play", "clientbound", "minecraft:container_set_data"), "play_container_set_data");
        set(ids.play_set_held_slot, find("play", "clientbound", "minecraft:set_held_slot"), "play_set_held_slot");
        set(ids.play_set_entity_data, find("play", "clientbound", "minecraft:set_entity_data"), "play_set_entity_data");
        set(ids.play_take_item_entity, find("play", "clientbound", "minecraft:take_item_entity"), "play_take_item_entity");

        // Entity tracking
        set(ids.play_add_entity, find("play", "clientbound", "minecraft:add_entity"), "play_add_entity");
        set(ids.play_remove_entities, find("play", "clientbound", "minecraft:remove_entities"), "play_remove_entities");
        set(ids.play_entity_position_sync, find("play", "clientbound", "minecraft:entity_position_sync"), "play_entity_position_sync");
        set(ids.play_move_entity_pos_rot, find("play", "clientbound", "minecraft:move_entity_pos_rot"), "play_move_entity_pos_rot");
        set(ids.play_move_entity_pos, find("play", "clientbound", "minecraft:move_entity_pos"), "play_move_entity_pos");
        set(ids.play_rotate_head, find("play", "clientbound", "minecraft:rotate_head"), "play_rotate_head");
        set(ids.play_entity_event, find("play", "clientbound", "minecraft:entity_event"), "play_entity_event");
        set(ids.play_hurt_animation, find("play", "clientbound", "minecraft:hurt_animation"), "play_hurt_animation");
        set(ids.play_player_info_update, find("play", "clientbound", "minecraft:player_info_update"), "play_player_info_update");
        set(ids.play_player_info_remove, find("play", "clientbound", "minecraft:player_info_remove"), "play_player_info_remove");
        set(ids.play_block_update, find("play", "clientbound", "minecraft:block_update"), "play_block_update");
        set(ids.play_block_changed_ack, find("play", "clientbound", "minecraft:block_changed_ack"), "play_block_changed_ack");
        set(ids.play_section_blocks_update, find("play", "clientbound", "minecraft:section_blocks_update"), "play_section_blocks_update");

        LOG_DEBUG(Protocol, "resolved IDs: login=0x{:02X} pos=0x{:02X} event=0x{:02X} center=0x{:02X} chunk=0x{:02X}",
            ids.play_login, ids.play_player_position, ids.play_game_event,
            ids.play_set_center_chunk, ids.play_level_chunk);

        LOG_INFO(Protocol, "packet ids loaded from {}", path);
        return true;
    }

}