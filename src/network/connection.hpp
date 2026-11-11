#pragma once

#include <asio.hpp>
#include "log/logger.hpp"
#include "protocol/play.hpp"
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
#include <memory>
#include <array>
#include <cstdint>
#include <vector>
#include <deque>
#include <optional>
#include <string_view>

namespace terila::network {

    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        explicit Connection(asio::ip::tcp::socket socket, const terila::data::RegistryData& data)
            : socket_(std::move(socket)), data_(data)
        {}

        void start() {
            do_read();
        }

        void send(const std::vector<std::uint8_t>& data) {
            write_queue_.push_back(data);
            if (!writing_) {
                do_write();
            }
        }

    private:

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
                return;
            }

            if (error) {
                LOG_ERROR(Network, "read failed: {} ({})", error.message(), error.value());
                return;
            }

            if (bytes_transferred == 0) {
                LOG_ERROR(Network, "read 0 bytes without error — TCP invariant violated");
                return;
            }

            LOG_TRACE(Network, "read {} bytes, accum={}", bytes_transferred, read_accum_.size() + bytes_transferred);

            read_accum_.insert(read_accum_.end(),
                read_buffer_.begin(),
                read_buffer_.begin() + bytes_transferred);

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

                    std::array<std::uint8_t, 16> uuid{};
                    std::array<std::uint8_t, 16> session_id{};

                    std::vector<std::uint8_t> out;

                    if (!terila::protocol::write_login_success(out, result->username, uuid, session_id)) {
                        LOG_ERROR(Network, "write_login_success failed for username={}", result->username);
                        return;
                    }
                    send(out);
                    LOG_INFO(Network, "login success sent: username={}", result->username);
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
                if (id == ids.config_registry_data) {
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
                        return;
                    }
                    send(tags_out);
                    LOG_DEBUG(Registry, "update tags sent ({} bytes)", data_.tag_payload.size());

                    std::vector<std::uint8_t> finish_out;
                    terila::protocol::write_packet(finish_out, ids.config_finish, {});
                    send(finish_out);

                    LOG_DEBUG(Network, "configuration data sent, awaiting client ack");
                }
                else if (id == ids.config_finish) {
                    LOG_DEBUG(Network, "state: Configuration → Play");

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

                    terila::world::flat::FlatSpec spec;

                    // A single chunk is not enough to mesh: send the 3x3 area around spawn.
                    constexpr int kRadius = 1;
                    std::vector<std::vector<std::uint8_t>> chunks;
                    chunks.reserve((2 * kRadius + 1) * (2 * kRadius + 1));
                    for (int cz = -kRadius; cz <= kRadius; ++cz) {
                        for (int cx = -kRadius; cx <= kRadius; ++cx) {
                            auto c = terila::world::flat::generate(cx, cz, spec, resolve_block, resolve_biome);
                            if (!c) {
                                LOG_ERROR(Network, "cannot generate chunk ({}, {})", cx, cz);
                                return;
                            }
                            chunks.push_back(terila::world::encode::encode(*c));
                        }
                    }
                    LOG_DEBUG(Chunk, "{} chunks encoded, {} bytes each", chunks.size(), chunks[4].size());


                    std::vector<std::uint8_t> out;

                    if (!terila::protocol::write_login_play(out, 76, false, 0, "minecraft:overworld")) {
                        LOG_ERROR(Network, "login_play failed"); return;
                    }
                    send(out); out.clear();

                    // Vanilla sends the teleport before the game event, then the cache
                    // center, then the chunks wrapped in a batch.
                    if (!terila::protocol::write_player_position(out, 1,
                        8.5, -59.0, 8.5, 0.0, 0.0, 0.0, 0.0f, 0.0f)) {
                        LOG_ERROR(Network, "player_position failed"); return;
                    }
                    send(out); out.clear();

                    if (!terila::protocol::write_game_event(out, 13, 0.0f)) {
                        LOG_ERROR(Network, "game_event failed"); return;
                    }
                    send(out); out.clear();

                    if (!terila::protocol::write_set_center_chunk(out, 0, 0)) {
                        LOG_ERROR(Network, "set_center_chunk failed"); return;
                    }
                    send(out); out.clear();

                    if (!terila::protocol::write_chunk_batch_start(out)) {
                        LOG_ERROR(Network, "chunk_batch_start failed"); return;
                    }
                    send(out); out.clear();

                    for (const auto& wire : chunks) {
                        if (!terila::protocol::write_chunk_data(out, wire)) {
                            LOG_ERROR(Network, "chunk_data failed"); return;
                        }
                        send(out); out.clear();
                    }

                    if (!terila::protocol::write_chunk_batch_finished(out, static_cast<std::int32_t>(chunks.size()))) {
                        LOG_ERROR(Network, "chunk_batch_finished failed"); return;
                    }
                    send(out);

                    state_ = State::Play;
                    LOG_INFO(Network, "client finished configuration");
                }
                break;

            case State::Play:
                if (id == 0x0D) break;
                LOG_DEBUG(Network, "unhandled play packet id=0x{:02X} ({} bytes)", id, payload.size());
                break;
            }
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
                        LOG_ERROR(Network, "write failed: {} ({})", error.message(), error.value());
                        return;
                    }

                    LOG_TRACE(Network, "wrote {} bytes", bytes_transferred);
                    self->write_queue_.pop_front();
                    self->do_write();
                }
            );
        }

        asio::ip::tcp::socket socket_;
        std::vector<std::uint8_t> read_accum_;
        std::array<std::uint8_t, 4096> read_buffer_;
        State state_ = State::Handshake;
        std::deque<std::vector<std::uint8_t>> write_queue_;
        bool writing_ = false;
        const terila::data::RegistryData& data_;
    };

} // namespace terila::network