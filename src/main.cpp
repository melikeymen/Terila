#include <asio.hpp>
#include "log/logger.hpp"
#include "network/connection.hpp"
#include "data/registry_data.hpp"
#include "data/registry_loader.hpp"
#include "config/server_config.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <filesystem>
#include <string>
#include <fstream>
#include <algorithm>

int main() {

    terila::data::RegistryData data;
    namespace fs = std::filesystem;

    auto base = fs::current_path();
    while (true) {
        std::error_code ec;
        if (fs::exists(base / "input", ec) && !ec) break;
        auto next = base.parent_path();
        if (next == base) break;
        base = next;
    }

    std::error_code ec;
    if (!fs::exists(base / "input", ec) || ec) {
        std::fprintf(stderr, "input/ not found. cwd=%s\n", fs::current_path().string().c_str());
        return 7;
    }

    terila::config::ServerConfig srv;
    terila::config::load(srv, (base / "server.properties").string());

    terila::log::Config cfg;
    if (auto lvl = terila::log::parse_level(srv.log_level)) {
        cfg.min_level = *lvl;
    }
    cfg.file_path = srv.log_file;
    terila::log::init(cfg);

    LOG_INFO(Main, "server starting");
    LOG_DEBUG(Main, "config: bind={} port={} log_level={}",
        srv.bind_address, srv.port, srv.log_level);

    const std::string input_dir = (base / "input").string();
    LOG_DEBUG(Main, "input dir: {}", input_dir);

    try {
        for (auto& entry : fs::recursive_directory_iterator(input_dir + "/registries/nbt")) {
            if (entry.path().extension() != ".nbt") continue;

            auto rel = entry.path().lexically_relative(input_dir + "/registries/nbt");
            std::string id = rel.generic_string();
            id = id.substr(0, id.size() - 4);

            auto slash = id.find('/');
            if (slash != std::string::npos) id[slash] = ':';

            terila::data::load_binary(data.nbt_payloads[id], entry.path().string());
        }
    }
    catch (const fs::filesystem_error& e) {
        LOG_FATAL(Registry, "cannot iterate nbt dir: {}", e.what());
        return 7;
    }

    terila::data::load_binary(data.tag_payload, input_dir + "/tags/client_tags.bin");

    std::ifstream idx_file(input_dir + "/registries/index.json");
    if (!idx_file) {
        LOG_FATAL(Registry, "index.json not found: {}", input_dir + "/registries/index.json");
        return 7;
    }
    auto idx = nlohmann::json::parse(idx_file);

    for (const auto& entry : idx["index"]) {
        if (entry["source"] == "datapack") {
            data.datapack_registries.push_back(entry["id"].get<std::string>());
        }
    }

    LOG_DEBUG(Registry, "datapack registries: {}", data.datapack_registries.size());

    const std::vector<std::string> vanilla_registries = {
        "minecraft:worldgen/biome",
        "minecraft:painting_variant",
        "minecraft:damage_type",
        "minecraft:banner_pattern",
        "minecraft:enchantment",
        "minecraft:jukebox_song",
        "minecraft:decorated_pot_pattern",
        "minecraft:chat_type",
        "minecraft:trim_pattern",
        "minecraft:trim_material",
        "minecraft:wolf_variant",
        "minecraft:wolf_sound_variant",
        "minecraft:pig_variant",
        "minecraft:pig_sound_variant",
        "minecraft:frog_variant",
        "minecraft:cat_variant",
        "minecraft:cat_sound_variant",
        "minecraft:cow_sound_variant",
        "minecraft:cow_variant",
        "minecraft:chicken_sound_variant",
        "minecraft:chicken_variant",
        "minecraft:zombie_nautilus_variant",
        "minecraft:sulfur_cube_archetype",
        "minecraft:dimension_type",
        "minecraft:test_environment",
        "minecraft:test_instance",
        "minecraft:dialog",
        "minecraft:instrument",
        "minecraft:world_clock",
        "minecraft:timeline",
        "minecraft:block_transformer",
        "minecraft:worldgen/block_state_provider",
    };

    std::vector<std::string> filtered_regs;
    for (const auto& id : data.datapack_registries) {
        bool is_vanilla = std::find(vanilla_registries.begin(),
            vanilla_registries.end(), id)
            != vanilla_registries.end();
        if (is_vanilla) {
            filtered_regs.push_back(id);
        }
    }

    data.datapack_registries = filtered_regs;
    LOG_DEBUG(Registry, "filtered registries: {}", data.datapack_registries.size());

    terila::data::load_binary(data.recipe_payload, input_dir + "/recipes/minecraft.nbt");
    terila::data::load_binary(data.loot_payload, input_dir + "/loot/minecraft.nbt");
    terila::data::load_binary(data.advancement_payload, input_dir + "/advancements/minecraft.nbt");
    terila::data::load_command_tree(data.command_tree_payload, input_dir + "/commands/tree.nbt");

    LOG_DEBUG(Registry, "loaded {} registry payloads", data.nbt_payloads.size());
    LOG_DEBUG(Registry, "tags: {} bytes", data.tag_payload.size());
    LOG_DEBUG(Registry, "recipes: {} bytes", data.recipe_payload.size());
    LOG_DEBUG(Registry, "loot: {} bytes", data.loot_payload.size());
    LOG_DEBUG(Registry, "advancements: {} bytes", data.advancement_payload.size());
    LOG_DEBUG(Registry, "command tree: {} bytes", data.command_tree_payload.size());

    asio::io_context io;
    asio::ip::address bind_addr = asio::ip::make_address(srv.bind_address);
    asio::ip::tcp::endpoint endpoint(bind_addr, srv.port);
    asio::ip::tcp::acceptor acceptor(io, endpoint);

    LOG_INFO(Network, "listening on {}:{}", srv.bind_address, srv.port);

    auto do_accept = [&](auto&& self) -> void {
        acceptor.async_accept(
            [&acceptor, &data, self](const asio::error_code& error, asio::ip::tcp::socket socket) {
                if (error) {
                    LOG_ERROR(Network, "accept failed: {} ({})", error.message(), error.value());
                    self(self);
                    return;
                }

                asio::error_code ep_ec;
                auto endpoint = socket.remote_endpoint(ep_ec);
                if (!ep_ec) {
                    LOG_INFO(Network, "new connection from {}:{}",
                        endpoint.address().to_string(), endpoint.port());
                }
                else {
                    LOG_INFO(Network, "new connection (endpoint unavailable: {})", ep_ec.message());
                }

                auto conn = std::make_shared<terila::network::Connection>(std::move(socket), data);
                conn->start();
                self(self);
            }
        );
        };

    do_accept(do_accept);
    io.run();

    terila::log::shutdown();
    return 0;
}