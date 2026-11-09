#pragma once

#include <asio.hpp>
#include "log/logger.hpp"
#include "protocol/login.hpp"
#include "protocol/packet.hpp"
#include "protocol/handshake.hpp"
#include "protocol/status.hpp"
#include "data/registry_data.hpp"
#include <memory>
#include <array>
#include <cstdint>
#include <vector>
#include <deque>

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
                LOG_ERROR(Network, "read 0 bytes without error — TCP invariant violated (possible Asio bug or race condition)"); 
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
                if (id == 0x00) {
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
                else if (id == 0x01) {
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
                if (id == 0x00) {
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
                else if (id == 0x03) {
                    LOG_DEBUG(Network, "state: Login → Configuration");
                    state_ = State::Configuration;

                    std::vector<std::uint8_t> features_payload;
                    terila::protocol::write_varint(features_payload, 1);
                    terila::protocol::write_string(features_payload, "minecraft:vanilla");
                    std::vector<std::uint8_t> features_out;
                    terila::protocol::write_packet(features_out, 0x0D, features_payload);
                    send(features_out);
                    LOG_DEBUG(Network, "update enabled features sent");

                    std::vector<std::uint8_t> select_payload;
                    terila::protocol::write_varint(select_payload, 1);
                    terila::protocol::write_string(select_payload, "minecraft");
                    terila::protocol::write_string(select_payload, "core");
                    terila::protocol::write_string(select_payload, "26.3");

                    std::vector<std::uint8_t> select_out;
                    terila::protocol::write_packet(select_out, 0x0F, select_payload);
                    send(select_out);
                    LOG_DEBUG(Network, "select known packs sent");
                }
                else {
                    LOG_DEBUG(Network, "unknown login packet id=0x{:02X}", id);
                }
                break;

            case State::Configuration:
                if (id == 0x07) {
                    LOG_DEBUG(Registry, "sending configuration data: {} registries", data_.datapack_registries.size());

                    int count = 0;
                    for (const auto& registry_id : data_.datapack_registries) {
                        auto it = data_.nbt_payloads.find(registry_id);
                        if (it == data_.nbt_payloads.end()) {
                            LOG_WARN(Registry, "missing payload for registry: {}", registry_id);
                            continue;
                        }

                        std::vector<std::uint8_t> out;
                        if (!terila::protocol::write_packet(out, 0x07, it->second)) {
                            LOG_ERROR(Registry, "write_packet failed for registry: {}", registry_id);
                            continue;
                        }
                        send(out);
                        ++count;
                    }
                    LOG_DEBUG(Registry, "sent {} registry data packets", count);

                    std::vector<std::uint8_t> tags_out;
                    if (!terila::protocol::write_packet(tags_out, 0x0E, data_.tag_payload)) {
                        LOG_ERROR(Registry, "write_packet failed for tags ({} bytes)", data_.tag_payload.size());
                        return;
                    }
                    send(tags_out);
                    LOG_DEBUG(Registry, "update tags sent ({} bytes)", data_.tag_payload.size());

                    std::vector<std::uint8_t> finish_out;
                    terila::protocol::write_packet(finish_out, 0x03, {});
                    send(finish_out);

                    LOG_DEBUG(Network, "configuration data sent, awaiting client ack");
                }
                else if (id == 0x03) {
                    LOG_DEBUG(Network, "state: Configuration → Play");
                    state_ = State::Play;
                    LOG_INFO(Network, "client finished configuration");
                }
                else {
                    LOG_DEBUG(Network, "unknown configuration packet id=0x{:02X}", id);
                }
                break;

            case State::Play:
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