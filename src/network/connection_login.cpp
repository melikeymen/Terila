#include "network/connection.hpp"

#include "network/mojang_profile.hpp"

#include "protocol/command_tree.hpp"

#include "protocol/login.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/play.hpp"
#include "protocol/primitives.hpp"
#include "protocol/uuid.hpp"

#include <algorithm>
#include <array>

#include "version/features.hpp"
#include "version/mc_version.hpp"

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

            // The client sends the profile it plays with, and a premium launcher sends the
            // account's real UUID: taking that makes a premium login instant instead of
            // paying a round trip to Mojang that, on a slow route, did not even come back
            // before the timeout and silently handed a real account an offline identity.
            // It is the same trust the name already has — anybody can claim any name until
            // session auth exists — and a client that sends nothing still gets the lookup.
            const bool claimed = result->has_uuid &&
                std::any_of(result->uuid.begin(), result->uuid.end(),
                    [](std::uint8_t byte) { return byte != 0; });

            terila::network::PlayerProfile profile;
            std::array<std::uint8_t, 16> uuid{};
            if (claimed) {
                uuid = result->uuid;
            }
            else if (const auto looked_up =
                         terila::network::lookup_mojang_profile(result->username);
                     looked_up.found) {
                profile = looked_up;
                uuid = looked_up.uuid;
            }
            else if (const auto offline = terila::protocol::offline_uuid(result->username)) {
                uuid = *offline;
            }
            else {
                // Without a digest every player would share one identity, so the login stops
                // here instead of joining with an invented UUID.
                LOG_ERROR(Network, "no UUID available for {}, refusing the login",
                    result->username);
                close();
                return;
            }
            uuid_ = uuid;
            profile_properties_ = profile.properties;
            std::array<std::uint8_t, 16> session_id{};
            username_ = profile.found && !profile.name.empty() ? profile.name
                : result->username;

            // One line per attempt: who is connecting, from where, with which identity.
            LOG_INFO(Network, "{}[/{}] logging in (uuid {}, {})", username_, remote_,
                terila::protocol::uuid_to_string(uuid),
                claimed ? "client profile" : profile.found ? "account" : "offline identity");

            std::vector<std::uint8_t> out;

            if (!terila::protocol::write_login_success(out, username_, uuid, session_id)) {
                LOG_ERROR(Network, "write_login_success failed for username={}", username_);
                close();
                return;
            }
            send(out);
            LOG_DEBUG(Network, "login success sent: username={}", result->username);
        }
        else if (id == ids.login_acknowledged) {
            LOG_DEBUG(Network, "state: Login → Configuration");
            state_ = State::Configuration;

#ifdef TERILA_HAS_UPDATE_ENABLED_FEATURES
            std::vector<std::uint8_t> features_payload;
            terila::protocol::write_varint(features_payload, 1);
            terila::protocol::write_string(features_payload, "minecraft:vanilla");
            std::vector<std::uint8_t> features_out;
            terila::protocol::write_packet(features_out, ids.config_update_enabled_features, features_payload);
            send(features_out);
            LOG_DEBUG(Network, "update enabled features sent");
#endif

#ifdef TERILA_HAS_SELECT_KNOWN_PACKS
            std::vector<std::uint8_t> select_payload;
            terila::protocol::write_varint(select_payload, 1);
            terila::protocol::write_string(select_payload, "minecraft");
            terila::protocol::write_string(select_payload, "core");
            // The pack version the client compares against its own; it follows the protocol
            // target instead of being pinned to the release this tree started on.
            terila::protocol::write_string(select_payload, std::string(terila::version::known_packs()));

            std::vector<std::uint8_t> select_out;
            terila::protocol::write_packet(select_out, ids.config_select_known_packs, select_payload);
            send(select_out);
            LOG_DEBUG(Network, "select known packs sent");
#endif
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

            init_terrain();
            if (!terrain_.ready()) {
                LOG_ERROR(Network, "terrain generator unavailable, closing the connection");
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
            const double spawn_x = spawn_x_ + 0.5;
            const double spawn_z = spawn_z_ + 0.5;
            const double spawn_y =
                static_cast<double>(terrain_.surface_height(spawn_x_, spawn_z_) + 1);
            if (!terila::protocol::write_player_position(out, next_teleport_id_++,
                spawn_x, spawn_y, spawn_z, 0.0, 0.0, 0.0, 0.0f, 0.0f)) {
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

            if (!terila::protocol::write_set_center_chunk(out,
                chunk_coordinate(spawn_x_), chunk_coordinate(spawn_z_))) {
                LOG_ERROR(Network, "set_center_chunk failed");
                close();
                return;
            }
            send(out);

            // Permissions, the starting gamemode and matching abilities (flight for
            // creative and spectator) before the first chunk arrives.
            send_gamemode_state();

            // Without the tree the client shows no command list at all: a bare "/" lists
            // nothing and every word the player types is flagged as unknown. The tree is
            // built from the commands this server actually runs, because the dump in
            // input/commands is not a tree the client accepts (it cycles).
            {
                // Parser ids come from the same registry dump the client uses; the fallbacks
                // are their ids in the versions that carry those parsers.
                const auto parser_id = [this](const char* name, std::int32_t fallback) {
                    const auto found = data_.command_argument_types.find(name);
                    return found == data_.command_argument_types.end() ? fallback : found->second;
                    };

                const auto nodes = terila::protocol::builtin_command_tree(
                    parser_id("minecraft:item_stack", 14), parser_id("brigadier:integer", 3),
                    parser_id("minecraft:entity", 6), parser_id("minecraft:message", 20));
                const auto tree = terila::protocol::write_command_tree(nodes, 0);
                if (!tree.empty() &&
                    terila::protocol::write_packet(out, ids.play_commands, tree)) {
                    send(out);
                    out.clear();
                    LOG_DEBUG(Protocol, "command tree sent: {} nodes, {} bytes",
                        nodes.size(), tree.size());
                }
                else {
                    LOG_ERROR(Protocol, "commands packet failed ({} nodes)", nodes.size());
                }
            }

            state_ = State::Play;
            joined_ = true;
            start_keep_alive();
            // The client starts at full health, but the server has to say so once: the value
            // it shows is the one set_health carried, not the one it guessed.
            send_health();
            send_time();
            LOG_INFO(Network, "{} joined the game", username_);

            // From here chunks stream in batches, both at spawn and while walking.
            center_cx_ = chunk_coordinate(spawn_x_);
            center_cz_ = chunk_coordinate(spawn_z_);
            player_x_ = spawn_x;
            player_y_ = spawn_y;
            player_z_ = spawn_z;
            queue_chunk_area(center_cx_, center_cz_);
            if (hub_) {
                const auto weak_self = weak_from_this();
                hub_->join(entity_id_, uuid_, username_, gamemode_, profile_properties_,
                    PlayerHub::Position{ player_x_, player_y_, player_z_, yaw_, pitch_ },
                    [weak_self](const std::vector<std::uint8_t>& packet) {
                        if (const auto connection = weak_self.lock()) {
                            connection->send(packet);
                        }
                    });
                hub_->set_giver(entity_id_,
                    [weak_self](std::int32_t item_id, std::int32_t count,
                        const terila::item::ComponentsPtr& components) {
                        if (const auto connection = weak_self.lock()) {
                            return connection->give_item(item_id, count, components);
                        }
                        return false;
                    });
                hub_->set_runner(entity_id_, [weak_self](const std::string& nested) {
                    if (const auto connection = weak_self.lock()) {
                        connection->run_command(nested);
                    }
                });
                // Mob hits arrive from the mob manager through the hub: it knows the entity
                // ids, this connection owns the health behind one of them.
                hub_->set_damager(entity_id_, [weak_self](float amount, const std::string& cause) {
                    if (const auto connection = weak_self.lock()) {
                        return connection->damage_player(amount, cause);
                    }
                    return false;
                });
            }
        }
    }

} // namespace terila::network
