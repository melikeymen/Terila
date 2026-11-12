#pragma once

#include <asio.hpp>
#include "log/logger.hpp"
#include "protocol/play.hpp"
#include "protocol/primitives.hpp"
#include "protocol/uuid.hpp"
#include "protocol/login.hpp"
#include "protocol/packet.hpp"
#include "protocol/handshake.hpp"
#include "protocol/status.hpp"
#include "protocol/packet_ids.hpp"
#include "data/registry_data.hpp"
#include "world/chunk.hpp"
#include "world/chunk_decoder.hpp"
#include "world/chunk_encoder.hpp"
#include "world/flat_generator.hpp"
#include "protocol/entity.hpp"
#include "entity/mob.hpp"
#include <memory>
#include <array>
#include <cstdint>
#include <vector>
#include <deque>
#include <optional>
#include <string_view>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace terila::network {

    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        explicit Connection(asio::ip::tcp::socket socket, const terila::data::RegistryData& data,
            terila::entity::MobManager& mobs, int view_distance, int simulation_distance,
            int max_players, std::int32_t entity_id)
            : socket_(std::move(socket)), keep_alive_timer_(socket_.get_executor()), data_(data),
              mobs_(mobs), view_distance_(view_distance),
              simulation_distance_(simulation_distance), max_players_(max_players),
              entity_id_(entity_id)
        {}

        void start() {
            asio::error_code ec;
            const auto remote = socket_.remote_endpoint(ec);
            if (!ec) {
                remote_ = remote.address().to_string() + ":" + std::to_string(remote.port());
            }
            do_read();
        }

        void send(const std::vector<std::uint8_t>& data) {
            if (closed_) {
                return;
            }
            write_queue_.push_back(data);
            if (!writing_) {
                do_write();
            }
        }

        // Called by the server tick for every connected player.
        void tick_entities() {
            send_entity_updates(mobs_.mobs());
        }

        bool joined() const { return joined_; }

        terila::entity::PlayerPos player_position() const {
            return terila::entity::PlayerPos{ player_x_, player_y_, player_z_ };
        }

    private:

        // A peer that never completes a packet would otherwise grow the accumulator forever.
        static constexpr std::size_t kMaxReadAccum = 4 * 1024 * 1024;

        // Chunks per batch; the next batch waits until the previous one is on the wire.
        static constexpr int kChunksPerBatch = 32;

        // Keep a few packets in flight instead of stalling on every single write.
        static constexpr std::size_t kMaxWriteQueue = 4;

        static constexpr std::int32_t kPlayerAttackDamage = 4;

        struct ChunkPos {
            std::int32_t x = 0;
            std::int32_t z = 0;
        };

        struct KnownEntity {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            float yaw = 0.0f;
            std::uint32_t hurt_serial = 0;
            bool death_sent = false;
        };

        enum class State {
            Handshake,
            Status,
            Login,
            Configuration,
            Play
        };

        static const char* state_name(State s) noexcept {
            switch (s) {
            case State::Handshake:     return "Handshake";
            case State::Status:        return "Status";
            case State::Login:         return "Login";
            case State::Configuration: return "Configuration";
            case State::Play:          return "Play";
            }
            return "Unknown";
        }

        void process_packets() {
            while (true) {
                auto packet = terila::protocol::read_packet(read_accum_.data(), read_accum_.size());
                if (!packet.has_value()) {
                    return;
                }

                handle_packet(packet->id, packet->payload);
                read_accum_.erase(read_accum_.begin(),
                    read_accum_.begin() + packet->consumed);
            }
        }

        void do_read() {
            auto self = shared_from_this();

            socket_.async_read_some(
                asio::buffer(read_buffer_),
                [self](const asio::error_code& error, std::size_t bytes) {
                    self->handle_read(error, bytes);
                }
            );
        }

        void handle_read(const asio::error_code& error, std::size_t bytes_transferred) {
            if (error == asio::error::eof) {
                LOG_DEBUG(Network, "client disconnected (eof)");
                close();
                return;
            }

            if (error) {
                if (error == asio::error::connection_reset || error == asio::error::connection_aborted) {
                    LOG_DEBUG(Network, "client disconnected ({})", error.message());
                }
                else {
                    LOG_WARN(Network, "read failed: {} ({})", error.message(), error.value());
                }
                close();
                return;
            }

            if (bytes_transferred == 0) {
                LOG_ERROR(Network, "read 0 bytes without error — TCP invariant violated");
                close();
                return;
            }

            LOG_TRACE(Network, "read {} bytes, accum={}", bytes_transferred, read_accum_.size() + bytes_transferred);

            read_accum_.insert(read_accum_.end(),
                read_buffer_.begin(),
                read_buffer_.begin() + bytes_transferred);

            if (read_accum_.size() > kMaxReadAccum) {
                LOG_ERROR(Network, "read buffer exceeded {} bytes, closing connection", kMaxReadAccum);
                close();
                return;
            }

            process_packets();

            do_read();
        }

        void handle_packet(std::int32_t id, const std::vector<std::uint8_t>& payload) {
            LOG_TRACE(Network, "packet id=0x{:02X} payload={} bytes in state {}",
                id, payload.size(), state_name(state_));

            const auto& ids = terila::protocol::packet_ids();

            switch (state_) {
            case State::Handshake: {
                auto hs = terila::protocol::read_handshake(read_accum_.data(), read_accum_.size());

                if (!hs.has_value()) {
                    LOG_WARN(Network, "invalid handshake packet");
                    return;
                }

                LOG_DEBUG(Network, "handshake: protocol={} address={} port={} next_state={}",
                    hs->protocol, hs->address, hs->port, hs->next_state);

                if (hs->next_state == 1) {
                    state_ = State::Status;
                    LOG_DEBUG(Network, "state: Handshake → Status");
                }
                else if (hs->next_state == 2) {
                    state_ = State::Login;
                    LOG_DEBUG(Network, "state: Handshake → Login");
                }
                else {
                    LOG_WARN(Network, "invalid next_state={} in handshake", hs->next_state);
                }
                break;
            }

            case State::Status:
                if (id == ids.status_request) {
                    if (!terila::protocol::read_status_request(read_accum_.data(), read_accum_.size())) {
                        LOG_WARN(Network, "invalid status request");
                        return;
                    }

                    terila::protocol::StatusResponse resp;
                    resp.version_name = "26.3";
                    resp.version_protocol = 777;
                    resp.players_max = 20;
                    resp.players_online = 0;
                    resp.description = "Terila Server";
                    resp.favicon = "";

                    std::vector<std::uint8_t> out;
                    if (terila::protocol::write_status_response(out, resp)) {
                        send(out);
                        LOG_DEBUG(Network, "status response sent ({} bytes)", out.size());
                        break;
                    }
                    LOG_ERROR(Network, "write_status_response failed");
                }
                else if (id == ids.ping) {
                    auto ping_value = terila::protocol::read_ping(read_accum_.data(), read_accum_.size());
                    if (!ping_value.has_value()) {
                        LOG_WARN(Network, "invalid ping packet");
                        return;
                    }

                    std::vector<std::uint8_t> out;
                    terila::protocol::write_pong(out, *ping_value);
                    send(out);
                    LOG_TRACE(Network, "pong sent, value={}", *ping_value);
                }
                else {
                    LOG_DEBUG(Network, "unknown status packet id=0x{:02X}", id);
                }
                break;

            case State::Login:
                if (id == ids.login_start) {
                    auto result = terila::protocol::read_login_start(read_accum_.data(), read_accum_.size());

                    if (!result.has_value()) {
                        LOG_WARN(Network, "invalid login start packet");
                        return;
                    }

                    const auto uuid = terila::protocol::offline_uuid(result->username);
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
                break;

            case State::Configuration:
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

                    if (!terila::protocol::write_login_play(out, entity_id_, false, 0,
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
                }
                break;

            case State::Play:
                if (id == ids.play_keep_alive_serverbound) {
                    auto value = terila::protocol::read_int64(payload.data(), payload.size());
                    if (!value.has_value()) {
                        LOG_WARN(Network, "invalid keep_alive reply ({} bytes)", payload.size());
                        break;
                    }
                    if (*value != pending_keep_alive_id_) {
                        LOG_DEBUG(Network, "keep_alive reply id={} does not match sent id={}",
                            *value, pending_keep_alive_id_);
                    }
                    else {
                        keep_alive_answered_ = true;
                        LOG_DEBUG(Network, "keep_alive reply: id={}", *value);
                    }
                    break;
                }
                if (id == ids.play_attack) {
                    const auto target = terila::protocol::read_varint(payload.data(), payload.size());
                    if (!target.has_value()) {
                        LOG_WARN(Network, "invalid attack packet ({} bytes)", payload.size());
                        break;
                    }

                    const auto* mob = mobs_.find(target->value);
                    if (mob == nullptr) {
                        LOG_DEBUG(Network, "attack on unknown entity id={}", target->value);
                        break;
                    }

                    const float away = terila::entity::yaw_between(
                        player_x_, player_z_, mob->x, mob->z);
                    mobs_.damage(target->value, kPlayerAttackDamage, away);
                    break;
                }
                if (id == ids.play_move_player_pos || id == ids.play_move_player_pos_rot) {
                    const auto x = terila::protocol::read_double(payload.data(), payload.size(), 0);
                    const auto y = terila::protocol::read_double(payload.data(), payload.size(), 8);
                    const auto z = terila::protocol::read_double(payload.data(), payload.size(), 16);
                    if (x.has_value() && y.has_value() && z.has_value()) {
                        player_x_ = *x;
                        player_y_ = *y;
                        player_z_ = *z;
                        LOG_TRACE(Network, "player moved to ({:.2f}, {:.2f}, {:.2f})", *x, *y, *z);
                        update_player_chunk(*x, *z);
                    }
                    break;
                }
                LOG_DEBUG(Network, "unhandled play packet id=0x{:02X} ({} bytes)", id, payload.size());
                break;
            }
        }

        void close() {
            if (closed_) {
                return;
            }
            closed_ = true;

            if (joined_) {
                LOG_INFO(Network, "{} left the game", username_);
            }
            else if (!username_.empty()) {
                LOG_DEBUG(Network, "{}[/{}] disconnected before joining the game", username_, remote_);
            }

            asio::error_code ignored;
            keep_alive_timer_.cancel(ignored);
            socket_.close(ignored);
        }

        static std::uint64_t chunk_key(std::int32_t cx, std::int32_t cz) noexcept {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
                static_cast<std::uint32_t>(cz);
        }

        // Flat chunks are identical except for their coordinates, so one encode is enough.
        void build_chunk_template() {
            if (!chunk_template_.empty()) {
                return;
            }

            auto resolve_block = [this](std::string_view name) -> std::optional<std::int32_t> {
                auto it = data_.block_state_ids.find(std::string(name));
                if (it == data_.block_state_ids.end()) return std::nullopt;
                return it->second;
                };
            auto resolve_biome = [this](std::string_view name) -> std::optional<std::int32_t> {
                auto it = data_.biome_ids.find(std::string(name));
                if (it == data_.biome_ids.end()) return std::nullopt;
                return it->second;
                };

            spec_ = terila::world::flat::FlatSpec{};
            spec_.min_y = data_.overworld.min_y;
            spec_.section_count = data_.overworld.height / 16;

            const auto chunk = terila::world::flat::generate(0, 0, spec_, resolve_block, resolve_biome);
            if (!chunk) {
                LOG_ERROR(Chunk, "cannot generate the flat chunk template");
                return;
            }

            chunk_template_ = terila::world::encode::encode(*chunk);
            LOG_DEBUG(Chunk, "chunk template encoded: {} bytes", chunk_template_.size());
        }

        std::vector<std::uint8_t> chunk_at(std::int32_t cx, std::int32_t cz) const {
            std::vector<std::uint8_t> out = chunk_template_;
            for (int i = 0; i < 4; ++i) {
                out[i] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(cx) >> ((3 - i) * 8)) & 0xFF);
                out[4 + i] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(cz) >> ((3 - i) * 8)) & 0xFF);
            }
            return out;
        }

        void queue_chunk_area(std::int32_t center_cx, std::int32_t center_cz) {
            // Rings go outwards, so the ground under the player arrives first.
            for (int ring = 0; ring <= view_distance_; ++ring) {
                for (int dz = -ring; dz <= ring; ++dz) {
                    for (int dx = -ring; dx <= ring; ++dx) {
                        if (ring > 0 && std::max(std::abs(dx), std::abs(dz)) != ring) continue;

                        const std::int32_t cx = center_cx + dx;
                        const std::int32_t cz = center_cz + dz;
                        if (!sent_chunks_.insert(chunk_key(cx, cz)).second) continue;
                        pending_chunks_.push_back(ChunkPos{ cx, cz });
                    }
                }
            }

            LOG_DEBUG(Chunk, "chunks queued around ({}, {}): {} waiting", center_cx, center_cz,
                pending_chunks_.size());
            pump_chunks();
        }

        // Chunks left far behind are forgotten so they are streamed again on the way back.
        void forget_far_chunks(std::int32_t center_cx, std::int32_t center_cz) {
            const int limit = view_distance_ + 2;
            for (auto it = sent_chunks_.begin(); it != sent_chunks_.end();) {
                const auto cx = static_cast<std::int32_t>(*it >> 32);
                const auto cz = static_cast<std::int32_t>(*it & 0xFFFFFFFFu);
                if (std::abs(cx - center_cx) > limit || std::abs(cz - center_cz) > limit) {
                    it = sent_chunks_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        void pump_chunks() {
            if (pending_chunks_.empty() || write_queue_.size() >= kMaxWriteQueue) {
                return;
            }

            std::vector<std::uint8_t> out;
            if (!terila::protocol::write_chunk_batch_start(out)) return;
            send(out);
            out.clear();

            int count = 0;
            while (!pending_chunks_.empty() && count < kChunksPerBatch) {
                const auto pos = pending_chunks_.front();
                pending_chunks_.pop_front();

                if (!terila::protocol::write_chunk_data(out, chunk_at(pos.x, pos.z))) {
                    return;
                }
                send(out);
                out.clear();
                ++count;
            }

            if (!terila::protocol::write_chunk_batch_finished(out, count)) return;
            send(out);

            LOG_DEBUG(Chunk, "chunk batch sent: {} chunks, {} waiting", count, pending_chunks_.size());
        }

        void update_player_chunk(double x, double z) {
            const auto cx = static_cast<std::int32_t>(std::floor(x / 16.0));
            const auto cz = static_cast<std::int32_t>(std::floor(z / 16.0));
            if (cx == center_cx_ && cz == center_cz_) {
                return;
            }

            center_cx_ = cx;
            center_cz_ = cz;
            LOG_TRACE(Network, "chunk center moved to ({}, {})", cx, cz);

            std::vector<std::uint8_t> out;
            if (terila::protocol::write_set_center_chunk(out, cx, cz)) {
                send(out);
            }

            forget_far_chunks(cx, cz);
            queue_chunk_area(cx, cz);
        }

        // Spawns and removes mobs as they enter or leave the view distance and sends
        // relative movement for the ones already known to the client.
        void send_entity_updates(const std::vector<terila::entity::Mob>& mobs) {
            if (state_ != State::Play || closed_) {
                return;
            }

            const double range = static_cast<double>(view_distance_) * 16.0;
            const double range_sq = range * range;

            std::unordered_set<std::int32_t> alive;
            alive.reserve(mobs.size());

            std::vector<std::uint8_t> out;

            for (const auto& mob : mobs) {
                alive.insert(mob.id);

                const double dx = mob.x - player_x_;
                const double dz = mob.z - player_z_;
                const bool in_range = (dx * dx + dz * dz) <= range_sq;

                const auto known = known_entities_.find(mob.id);

                if (!in_range) {
                    if (known != known_entities_.end()) {
                        std::vector<std::uint8_t> remove;
                        if (terila::protocol::write_remove_entity(remove, mob.id)) {
                            send(remove);
                        }
                        known_entities_.erase(known);
                    }
                    continue;
                }

                if (known == known_entities_.end()) {
                    if (terila::protocol::write_add_entity(out, mob.id, mob.uuid, mob.type_id,
                        mob.x, mob.y, mob.z, mob.yaw, 0.0f, mob.head_yaw)) {
                        if (!logged_first_entity_) {
                            logged_first_entity_ = true;
                            std::string hex;
                            for (const auto byte : out) {
                                char buf[4];
                                std::snprintf(buf, sizeof(buf), "%02X ", byte);
                                hex += buf;
                            }
                            LOG_DEBUG(Entity, "add_entity type={} bytes={} [{}]",
                                mob.type_id, out.size(), hex);
                        }
                        send(out);
                    }
                    out.clear();
                    known_entities_[mob.id] = KnownEntity{ mob.x, mob.y, mob.z, mob.yaw };
                    continue;
                }

                // Hurt feedback is per viewer: one animation per hit serial.
                if (mob.hurt_serial != known->second.hurt_serial) {
                    known->second.hurt_serial = mob.hurt_serial;
                    const float yaw = terila::entity::yaw_between(
                        player_x_, player_z_, mob.x, mob.z);
                    if (terila::protocol::write_hurt_animation(out, mob.id, yaw)) {
                        send(out);
                    }
                    out.clear();
                    if (terila::protocol::write_entity_event(out, mob.id, 2)) {
                        send(out);
                    }
                    out.clear();
                }

                if (mob.dead) {
                    if (!known->second.death_sent) {
                        known->second.death_sent = true;
                        if (terila::protocol::write_entity_event(out, mob.id, 3)) {
                            send(out);
                        }
                        out.clear();
                    }
                    continue;
                }

                const auto to_short = [](double delta) {
                    const double scaled = delta * 4096.0;
                    if (scaled > 32767.0) return static_cast<std::int16_t>(32767);
                    if (scaled < -32768.0) return static_cast<std::int16_t>(-32768);
                    return static_cast<std::int16_t>(std::lround(scaled));
                    };

                const auto dxs = to_short(mob.x - known->second.x);
                const auto dys = to_short(mob.y - known->second.y);
                const auto dzs = to_short(mob.z - known->second.z);

                const bool moved = dxs != 0 || dys != 0 || dzs != 0;
                const bool turned = mob.yaw != known->second.yaw;

                if (!moved && !turned) {
                    continue;
                }

                if (moved) {
                    if (terila::protocol::write_move_entity_pos_rot(out, mob.id, dxs, dys, dzs,
                        mob.yaw, 0.0f, true)) {
                        send(out);
                    }
                    out.clear();
                }
                else if (turned) {
                    if (terila::protocol::write_rotate_head(out, mob.id, mob.head_yaw)) {
                        send(out);
                    }
                    out.clear();
                }

                known->second.x = mob.x;
                known->second.y = mob.y;
                known->second.z = mob.z;
                known->second.yaw = mob.yaw;
            }

            for (auto it = known_entities_.begin(); it != known_entities_.end();) {
                if (alive.find(it->first) != alive.end()) {
                    ++it;
                    continue;
                }

                std::vector<std::uint8_t> remove;
                if (terila::protocol::write_remove_entity(remove, it->first)) {
                    send(remove);
                }
                it = known_entities_.erase(it);
            }
        }

        void start_keep_alive() {
            auto self = shared_from_this();
            keep_alive_timer_.expires_after(std::chrono::seconds(15));
            keep_alive_timer_.async_wait([self](const asio::error_code& error) {
                if (error || self->closed_) {
                    return;
                }

                std::vector<std::uint8_t> out;
                const std::int64_t id = self->next_keep_alive_id_++;
                if (terila::protocol::write_keep_alive(out, id)) {
                    if (!self->keep_alive_answered_ && self->pending_keep_alive_id_ != 0) {
                        // Vanilla drops a client that misses a whole ping interval.
                        LOG_WARN(Network, "keep_alive id={} was never answered, closing the connection",
                            self->pending_keep_alive_id_);
                        self->close();
                        return;
                    }
                    self->pending_keep_alive_id_ = id;
                    self->keep_alive_answered_ = false;
                    self->send(out);
                    LOG_DEBUG(Network, "keep_alive sent: id={}", id);
                }
                self->start_keep_alive();
            });
        }

        void do_write() {
            if (write_queue_.empty()) {
                writing_ = false;
                return;
            }
            writing_ = true;

            auto self = shared_from_this();
            asio::async_write(
                socket_,
                asio::buffer(write_queue_.front()),
                [self](const asio::error_code& error, std::size_t bytes_transferred) {
                    if (error) {
                        LOG_DEBUG(Network, "write failed: {} ({})", error.message(), error.value());
                        self->close();
                        return;
                    }

                    LOG_TRACE(Network, "wrote {} bytes", bytes_transferred);
                    self->write_queue_.pop_front();
                    self->do_write();
                    self->pump_chunks();
                }
            );
        }

        asio::ip::tcp::socket socket_;
        std::vector<std::uint8_t> read_accum_;
        std::array<std::uint8_t, 4096> read_buffer_;
        State state_ = State::Handshake;
        std::deque<std::vector<std::uint8_t>> write_queue_;
        bool writing_ = false;
        bool closed_ = false;
        asio::steady_timer keep_alive_timer_;
        std::int64_t next_keep_alive_id_ = 1;
        std::int32_t next_teleport_id_ = 1;
        std::int64_t pending_keep_alive_id_ = 0;
        bool keep_alive_answered_ = true;
        std::string username_;
        std::string remote_;
        bool joined_ = false;
        bool logged_first_entity_ = false;
        int view_distance_ = 10;
        int simulation_distance_ = 10;
        int max_players_ = 20;
        std::int32_t entity_id_ = 76;
        std::int32_t center_cx_ = 0;
        std::int32_t center_cz_ = 0;
        terila::world::flat::FlatSpec spec_;
        std::vector<std::uint8_t> chunk_template_;
        std::deque<ChunkPos> pending_chunks_;
        std::unordered_set<std::uint64_t> sent_chunks_;
        std::unordered_map<std::int32_t, KnownEntity> known_entities_;
        double player_x_ = 8.5;
        double player_y_ = 0.0;
        double player_z_ = 8.5;
        const terila::data::RegistryData& data_;
        terila::entity::MobManager& mobs_;
    };

} // namespace terila::network