#pragma once

#include <asio.hpp>
#include <memory>
#include <array>
#include <cstdint>
#include <vector>
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


        void send(const std::vector<std::uint8_t>& data);

    private:
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

            do_read();
        }

        asio::ip::tcp::socket socket_;
        std::array<std::uint8_t, 4096> read_buffer_;
    };

} // namespace terila::network