#include <asio.hpp>
#include <iostream>
#include <memory>
#include "network/connection.hpp"

int main() {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 25565));

    auto do_accept = [&](auto&& self) -> void {
        acceptor.async_accept(
            [&acceptor, self](const asio::error_code& error, asio::ip::tcp::socket socket) {
                if (error) {
                    std::cerr << "[Accept] " << error.message() << "\n";
                    self(self);
                    return;
                }

                std::cout << "[Accept] new connection\n";

                auto conn = std::make_shared<terila::network::Connection>(std::move(socket));
                conn->start();
                self(self);
            }
        );
    };

    do_accept(do_accept);
    io.run();

    return 0;
}