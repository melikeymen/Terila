#include <asio.hpp>
#include "network/connection.hpp"
#include "data/registry_data.hpp"
#include "data/registry_loader.hpp"
#include <iostream>
#include <memory>
#include <filesystem>
#include <string>

int main() {

    terila::data::RegistryData data;

    namespace fs = std::filesystem;

    auto base = fs::current_path();
    while (!fs::exists(base / "input") && base.has_parent_path()) {
        base = base.parent_path();
    }

    if (!fs::exists(base / "input")) {
        std::cerr << "ERROR: input/ bulunamadi. Baslangic: " << fs::current_path() << "\n";
        return 1;
    }

    const std::string input_dir = (base / "input").string();
    std::cout << "Input dir: " << input_dir << "\n";

    for (auto& entry : fs::recursive_directory_iterator(input_dir + "/registries/nbt")) {
        if (entry.path().extension() != ".nbt") continue;

        auto rel = entry.path().lexically_relative(input_dir + "/registries/nbt");
        std::string id = rel.generic_string();
        id = id.substr(0, id.size() - 4);

        auto slash = id.find('/');
        if (slash != std::string::npos) id[slash] = ':';

        terila::data::load_binary(data.nbt_payloads[id], entry.path().string());
    }

    terila::data::load_binary(data.tag_payload, input_dir + "/tags/all.nbt");
    terila::data::load_binary(data.recipe_payload, input_dir + "/recipes/minecraft.nbt");
    terila::data::load_binary(data.loot_payload, input_dir + "/loot/minecraft.nbt");
    terila::data::load_binary(data.advancement_payload, input_dir + "/advancements/minecraft.nbt");
    terila::data::load_command_tree(data.command_tree_payload, input_dir + "/commands/tree.nbt");

    std::cout << "Loaded " << data.nbt_payloads.size() << " registry payloads\n";
    std::cout << "Tags: " << data.tag_payload.size() << " bytes\n";
    std::cout << "Recipes: " << data.recipe_payload.size() << " bytes\n";
    std::cout << "Loot: " << data.loot_payload.size() << " bytes\n";
    std::cout << "Advancements: " << data.advancement_payload.size() << " bytes\n";
    std::cout << "Command tree: " << data.command_tree_payload.size() << " bytes\n";

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