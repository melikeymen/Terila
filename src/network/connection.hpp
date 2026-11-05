#pragma once

#include <asio.hpp>
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
#include <iostream>

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
                [self](const asio::error_code& error, std::size_t bytes) { // callback
                    self->handle_read(error, bytes);
                }
            );
        }

        void handle_read(const asio::error_code& error, std::size_t bytes_transferred) {
            if (error == asio::error::eof) {
                std::cout << "[Connection] client disconnected (eof)\n";
                return;
            }

            if (error) {
                std::cerr << "[Connection] read: " << error.message()
                    << " (" << error.value() << ")\n";
                return;
            }

            if (bytes_transferred == 0) {
                std::cerr << "[Connection] read 0 bytes without error, closing (possible peer half-close)\n";
                return;
            }

            read_accum_.insert(read_accum_.end(),
                               read_buffer_.begin(),
                               read_buffer_.begin() + bytes_transferred);

            process_packets();

            do_read();
        }

        void handle_packet(std::int32_t id, const std::vector<std::uint8_t>& payload) {
            switch (state_) {
            case State::Handshake: {
                auto hs = terila::protocol::read_handshake(read_accum_.data(), read_accum_.size());

                if (!hs.has_value()) {
                    std::cerr << "[Connection] invalid handshake\n";
                    return;
                }

                std::cout << "[Connection] handshake: protocol=" << hs->protocol
                          << " address=" << hs->address
                          << " port=" << hs->port
                          << " next_state=" << hs->next_state << "\n";
                if (hs->next_state == 1) {
                    state_ = State::Status;
                } else if (hs->next_state == 2) {
                    state_ = State::Login;
                }
                else {
                    std::cerr << "[Connection] invalid next_state: " << hs->next_state << "\n";
                }
                break;
            }
                
            case State::Status:
                if (id == 0x00) {
                    if (!terila::protocol::read_status_request(read_accum_.data(), read_accum_.size())) {
                        std::cerr << "[Connection] invalid status request\n";
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
                        std::cout << "[Connection] status response sent\n";
                        break;
                    }


                }
                else if (id == 0x01) {
                    auto ping_value = terila::protocol::read_ping(read_accum_.data(), read_accum_.size());
                    if (!ping_value.has_value()) {
                        std::cerr << "[Connection] invalid ping\n";
                        return;
                    }

                    std::vector<std::uint8_t> out;

                    terila::protocol::write_pong(out, *ping_value);
                    send(out);
                    std::cout << "[Connection] pong sent\n";
                }

                else {
                    std::cerr << "[Connection] unknown status packet id: " << id << "\n";
                }
                break;

            case State::Login:
                if (id == 0x00) {
                    auto result = terila::protocol::read_login_start(read_accum_.data(), read_accum_.size());

                    if (!result.has_value()) {
                        return;
                    }

                    std::array<std::uint8_t, 16> uuid{};
                    std::array<std::uint8_t, 16> session_id{};

                    std::vector<std::uint8_t> out;

                    if (!terila::protocol::write_login_success(out, result->username, uuid, session_id)) {
                        std::cerr << "[Connection] write_login_success failed\n";
                        return;
                    }
                    send(out);
                    state_ = State::Configuration;
                    std::cout << "[Connection] login success sent for " << result->username << "\n";


                }

                break;
            case State::Configuration:              
                break;

            case State::Play:
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
                        std::cerr << "[Connection] write: " << error.message()
                            << " (" << error.value() << ")\n";

                        return;
                    }

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