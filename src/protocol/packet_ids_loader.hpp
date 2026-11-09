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

        // Play
        set(ids.play_login, find("play", "clientbound", "minecraft:login"), "play_login");
        set(ids.play_player_position, find("play", "clientbound", "minecraft:player_position"), "play_player_position");
        set(ids.play_game_event, find("play", "clientbound", "minecraft:game_event"), "play_game_event");
        set(ids.play_level_chunk, find("play", "clientbound", "minecraft:level_chunk_with_light"), "play_level_chunk");

        LOG_INFO(Protocol, "packet ids loaded from {}", path);
        return true;
    }

}