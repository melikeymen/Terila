#pragma once

#include <asio.hpp>
#include "protocol/packet.hpp"
#include <memory>
#include <array>
#include <cstdint>
#include <vector>
#include <deque>
#include <iostream>

namespace terila::network {
    
    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        explicit Connection(asio::ip::tcp::socket socket)
            : socket_(std::move(socket))
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
            case State::Handshake:
                break;
            case State::Status:
                break;
            case State::Login:
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
    };

} // namespace terila::network