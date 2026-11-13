#include "network/connection.hpp"

#include "protocol/login.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/play.hpp"
#include "protocol/primitives.hpp"
#include "protocol/uuid.hpp"

namespace terila::network {

    void Connection::handle_login(std::int32_t id, const std::vector<std::uint8_t>& payload) {
        (void)payload;

        const auto& ids = terila::protocol::packet_ids();

        if (id == ids.login_start) {
            auto result = terila::protocol::read_login_start(read_accum_.data(), read_accum_.size());

            if (!result.has_value()) {
                LOG_WARN(Network, "invalid login start packet");
                return;
            }

            const auto uuid = terila::protocol::offline_uuid(result->username);
            uuid_ = uuid;
            std::array<std::uint8_t, 16> session_id{};
            username_ = result->username;

            // One line per attempt: who is connecting, from where, with which identity.
            LOG_INFO(Network, "{}[/{}] logging in (uuid {})", username_, remote_,
                terila::protocol::uuid_to_string(uuid));

            std::vector<std::uint8_t> out;

            if (!terila::protocol::write_login_success(out, result->username, uuid, session_id)) {
                LOG_ERROR(Network, "write_login_success failed for username={}", result->username);
                close();
                return;
            }
            send(out);
            LOG_DEBUG(Network, "login success sent: username={}", result->username);
        }
        else if (id == ids.login_acknowledged) {
            LOG_DEBUG(Network, "state: Login → Configuration");
            state_ = State::Configuration;

            std::vector<std::uint8_t> features_payload;
            terila::protocol::write_varint(features_payload, 1);
            terila::protocol::write_string(features_payload, "minecraft:vanilla");
            std::vector<std::uint8_t> features_out;
            terila::protocol::write_packet(features_out, ids.config_update_enabled_features, features_payload);
            send(features_out);
            LOG_DEBUG(Network, "update enabled features sent");

            std::vector<std::uint8_t> select_payload;
            terila::protocol::write_varint(select_payload, 1);
            terila::protocol::write_string(select_payload, "minecraft");
            terila::protocol::write_string(select_payload, "core");
            terila::protocol::write_string(select_payload, "26.3");

            std::vector<std::uint8_t> select_out;
            terila::protocol::write_packet(select_out, ids.config_select_known_packs, select_payload);
            send(select_out);
            LOG_DEBUG(Network, "select known packs sent");
        }
        else {
            LOG_DEBUG(Network, "unknown login packet id=0x{:02X}", id);
        }
    }

    void Connection::handle_configuration(std::int32_t id, const std::vector<std::uint8_t>& payload) {
        const auto& ids = terila::protocol::packet_ids();

        if (id == ids.config_client_information_serverbound) {
            LOG_DEBUG(Network, "client information received ({} bytes)", payload.size());
        }
        else if (id == ids.config_select_known_packs_serverbound) {
            LOG_DEBUG(Registry, "sending configuration data: {} registries", data_.datapack_registries.size());

            int count = 0;
            for (const auto& registry_id : data_.datapack_registries) {
                auto it = data_.nbt_payloads.find(registry_id);
                if (it == data_.nbt_payloads.end()) {
                    LOG_WARN(Registry, "missing payload for registry: {}", registry_id);
                    continue;
                }

                std::vector<std::uint8_t> out;
                if (!terila::protocol::write_packet(out, ids.config_registry_data, it->second)) {
                    LOG_ERROR(Registry, "write_packet failed for registry: {}", registry_id);
                    continue;
                }
                send(out);
                ++count;
            }
            LOG_DEBUG(Registry, "sent {} registry data packets", count);

            std::vector<std::uint8_t> tags_out;
            if (!terila::protocol::write_packet(tags_out, ids.config_update_tags, data_.tag_payload)) {
                LOG_ERROR(Registry, "write_packet failed for tags ({} bytes)", data_.tag_payload.size());
                close();
                return;
            }
            send(tags_out);
            LOG_DEBUG(Registry, "update tags sent ({} bytes)", data_.tag_payload.size());

            std::vector<std::uint8_t> finish_out;
            if (!terila::protocol::write_packet(finish_out, ids.config_finish, {})) {
                LOG_ERROR(Network, "finish_configuration failed");
                close();
                return;
            }
            send(finish_out);

            LOG_DEBUG(Network, "configuration data sent, awaiting client ack");
        }
        else if (id == ids.config_finish_serverbound) {
            LOG_DEBUG(Network, "state: Configuration → Play");

            build_chunk_template();
            if (chunk_template_.empty()) {
                LOG_ERROR(Network, "chunk template unavailable, closing the connection");
                close();
                return;
            }

            std::vector<std::uint8_t> out;

            if (!terila::protocol::write_login_play(out, entity_id_, false,
                static_cast<std::uint8_t>(gamemode_),
                "minecraft:overworld", max_players_, view_distance_, simulation_distance_)) {
                LOG_ERROR(Network, "login_play failed");
                close();
                return;
            }
            send(out); out.clear();

            // Vanilla sends the teleport before the game event, then the cache
            // center, then the chunks wrapped in a batch.
            const double spawn_y =
                static_cast<double>(spec_.min_y + spec_.dirt_layers + 2);
            if (!terila::protocol::write_player_position(out, next_teleport_id_++,
                8.5, spawn_y, 8.5, 0.0, 0.0, 0.0, 0.0f, 0.0f)) {
                LOG_ERROR(Network, "player_position failed");
                close();
                return;
            }
            send(out); out.clear();

            if (!terila::protocol::write_game_event(out, 13, 0.0f)) {
                LOG_ERROR(Network, "game_event failed");
                close();
                return;
            }
            send(out); out.clear();

            if (!terila::protocol::write_set_center_chunk(out, 0, 0)) {
                LOG_ERROR(Network, "set_center_chunk failed");
                close();
                return;
            }
            send(out);

            // Permissions, the starting gamemode and matching abilities (flight for
            // creative and spectator) before the first chunk arrives.
            send_gamemode_state();

            state_ = State::Play;
            joined_ = true;
            start_keep_alive();
            LOG_INFO(Network, "{} joined the game", username_);

            // From here chunks stream in batches, both at spawn and while walking.
            center_cx_ = 0;
            center_cz_ = 0;
            player_x_ = 8.5;
            player_y_ = spawn_y;
            player_z_ = 8.5;
            queue_chunk_area(0, 0);
            if (hub_) {
                const auto weak_self = weak_from_this();
                hub_->join(entity_id_, uuid_, username_, gamemode_,
                    PlayerHub::Position{ player_x_, player_y_, player_z_, yaw_, pitch_ },
                    [weak_self](const std::vector<std::uint8_t>& packet) {
                        if (const auto connection = weak_self.lock()) {
                            connection->send(packet);
                        }
                    });
            }
        }
    }

} // namespace terila::network
