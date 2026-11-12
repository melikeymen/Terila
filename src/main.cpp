#include <asio.hpp>
#include <csignal>
#include "log/logger.hpp"
#include "network/connection.hpp"
#include "data/registry_data.hpp"
#include "data/registry_loader.hpp"
#include "config/server_config.hpp"
#include "protocol/packet_ids_loader.hpp"
#include "entity/mob.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <functional>
#include <vector>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

static fs::path find_input_dir() {
    auto base = fs::current_path();
    while (true) {
        std::error_code ec;
        if (fs::exists(base / "input", ec) && !ec) return base;
        auto next = base.parent_path();
        if (next == base) return {};
        base = next;
    }
}

int main() {
    auto base = find_input_dir();
    if (base.empty()) {
        const std::string cwd = fs::current_path().string();
        std::fprintf(stderr, "input/ not found. cwd=%s\n", cwd.c_str());
        return 7;
    }

    terila::config::ServerConfig srv;
    terila::config::load(srv, (base / "server.properties").string());

    if (srv.online_mode) {
        LOG_WARN(Main, "online-mode=true is not implemented yet, staying in offline mode");
        srv.online_mode = false;
    }
    if (srv.network_encryption) {
        LOG_WARN(Main, "network-encryption=true is not implemented yet, connections stay unencrypted");
        srv.network_encryption = false;
    }

    terila::log::Config cfg;
    if (auto lvl = terila::log::parse_level(srv.log_level)) cfg.min_level = *lvl;
    cfg.file_path = srv.log_file;
    terila::log::init(cfg);

    LOG_INFO(Main, "server starting");
    LOG_DEBUG(Main, "config: bind={} port={} log_level={} online_mode={} network_encryption={} view_distance={} simulation_distance={}",
        srv.bind_address, srv.port, srv.log_level, srv.online_mode, srv.network_encryption,
        srv.view_distance, srv.simulation_distance);

    const std::string in = (base / "input").string();
    if (!terila::protocol::load_packet_ids(in + "/packets/ids.json")) {
        LOG_FATAL(Protocol, "cannot load packet ids from {}/packets/ids.json", in);
        return 7;
    }

    terila::data::RegistryData data;

    // Registry NBT payloads
    try {
        for (auto& entry : fs::recursive_directory_iterator(in + "/registries/nbt")) {
            if (entry.path().extension() != ".nbt") continue;
            auto rel = entry.path().lexically_relative(in + "/registries/nbt");
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

    terila::data::load_binary(data.tag_payload, in + "/tags/client_tags.bin");

    // Datapack registry filter
    {
        std::ifstream idx_file(in + "/registries/index.json");
        if (!idx_file) {
            LOG_FATAL(Registry, "index.json not found: {}", in + "/registries/index.json");
            return 7;
        }
        auto idx = nlohmann::json::parse(idx_file);

        static const std::vector<std::string> keep = {
            "minecraft:worldgen/biome", "minecraft:painting_variant", "minecraft:damage_type",
            "minecraft:banner_pattern", "minecraft:enchantment", "minecraft:jukebox_song",
            "minecraft:decorated_pot_pattern", "minecraft:chat_type", "minecraft:trim_pattern",
            "minecraft:trim_material", "minecraft:wolf_variant", "minecraft:wolf_sound_variant",
            "minecraft:pig_variant", "minecraft:pig_sound_variant", "minecraft:frog_variant",
            "minecraft:cat_variant", "minecraft:cat_sound_variant", "minecraft:cow_sound_variant",
            "minecraft:cow_variant", "minecraft:chicken_sound_variant", "minecraft:chicken_variant",
            "minecraft:zombie_nautilus_variant", "minecraft:sulfur_cube_archetype", "minecraft:dimension_type",
            "minecraft:test_environment", "minecraft:test_instance", "minecraft:dialog",
            "minecraft:instrument", "minecraft:world_clock", "minecraft:timeline",
            "minecraft:block_transformer", "minecraft:worldgen/block_state_provider"
        };
        for (const auto& entry : idx["index"]) {
            if (entry["source"] != "datapack") continue;
            auto id = entry["id"].get<std::string>();
            if (std::find(keep.begin(), keep.end(), id) != keep.end())
                data.datapack_registries.push_back(id);
        }
    }

    terila::data::load_binary(data.recipe_payload, in + "/recipes/minecraft.nbt");
    terila::data::load_binary(data.loot_payload, in + "/loot/minecraft.nbt");
    terila::data::load_binary(data.advancement_payload, in + "/advancements/minecraft.nbt");
    terila::data::load_command_tree(data.command_tree_payload, in + "/commands/tree.nbt");

    terila::data::load_block_state_ids(data.block_state_ids, in + "/blocks/state_ids.json");
    terila::data::load_biome_ids(data.biome_ids, in + "/worldgen/biomes.json");
    terila::data::load_dimensions(data.overworld, in + "/worldgen/dimensions.json");
    terila::data::load_entity_type_ids(data.entity_type_ids, in + "/entities/minecraft.json");
    terila::data::load_entity_categories(data.entity_categories, in + "/entities/minecraft.json");

    if (data.block_state_ids.empty()) { LOG_FATAL(Data, "block_state_ids empty"); return 7; }
    if (data.biome_ids.empty()) { LOG_FATAL(Data, "biome_ids empty");       return 7; }
    if (data.entity_type_ids.empty()) {
        LOG_WARN(Data, "entity_type_ids empty, mobs stay disabled");
    }

    LOG_DEBUG(Registry, "datapack registries: {}", data.datapack_registries.size());
    LOG_DEBUG(Registry, "registry payloads: {}", data.nbt_payloads.size());

    terila::entity::MobManager mobs;
    mobs.set_ground(static_cast<double>(data.overworld.min_y + 5));
    // Every living entity type in the registry gets an instance, so the whole mob
    // catalogue is exercised instead of a hardcoded handful.
    static const std::unordered_set<std::string> kLivingCategories = {
        "creature", "monster", "ambient", "axolotls",
        "water_creature", "water_ambient", "underground_water_creature"
    };

    std::vector<std::string> living_types;
    for (const auto& entry : data.entity_type_ids) {
        const auto category = data.entity_categories.find(entry.first);
        if (category == data.entity_categories.end()) continue;
        if (kLivingCategories.find(category->second) == kLivingCategories.end()) continue;
        living_types.push_back(entry.first);
    }
    std::sort(living_types.begin(), living_types.end());

    std::size_t spawned = 0;
    for (std::size_t i = 0; i < living_types.size(); ++i) {
        const std::string& name = living_types[i];
        const auto category = data.entity_categories.at(name);
        const bool hostile = (category == "monster");

        const int column = static_cast<int>(i % 12);
        const int row = static_cast<int>(i / 12);
        const double x = 8.5 + (static_cast<double>(column) - 5.5) * 3.0;
        const double z = 8.5 + static_cast<double>(row) * 3.0;

        if (mobs.spawn(name, data.entity_type_ids, x, z, hostile, hostile ? 20 : 10)) {
            ++spawned;
        }
        else {
            LOG_WARN(Entity, "could not spawn {} at ({:.1f}, {:.1f})", name, x, z);
        }
    }
    LOG_INFO(Entity, "spawned {} of {} registered mob types", spawned, living_types.size());

    // Network
    asio::io_context io;
    std::error_code ec;

    asio::ip::address bind_addr = asio::ip::make_address(srv.bind_address, ec);
    if (ec) {
        LOG_FATAL(Network, "invalid bind address '{}': {}", srv.bind_address, ec.message());
        return 7;
    }

    asio::ip::tcp::endpoint endpoint(bind_addr, srv.port);
    asio::ip::tcp::acceptor acceptor(io);

    acceptor.open(endpoint.protocol(), ec);
    if (!ec) acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    if (!ec) acceptor.bind(endpoint, ec);
    if (!ec) acceptor.listen(asio::socket_base::max_listen_connections, ec);

    if (ec) {
        LOG_FATAL(Network, "failed to bind to {}:{} — {}", srv.bind_address, srv.port, ec.message());
        return 7;
    }

    LOG_INFO(Network, "listening on {}:{}", srv.bind_address, srv.port);

    std::vector<std::weak_ptr<terila::network::Connection>> players;

    // Mobs use 1..N, so player entities start well above them.
    std::int32_t next_player_entity_id = 1000;

    auto do_accept = [&](auto&& self) -> void {
        acceptor.async_accept(
            [&acceptor, &data, &mobs, &srv, &players, &next_player_entity_id, self](const asio::error_code& error, asio::ip::tcp::socket socket) {
                if (error) {
                    LOG_ERROR(Network, "accept failed: {} ({})", error.message(), error.value());
                    self(self);
                    return;
                }
                asio::error_code ep_ec;
                auto remote_ep = socket.remote_endpoint(ep_ec);
                if (!ep_ec) {
                    LOG_DEBUG(Network, "new connection from {}:{}", remote_ep.address().to_string(), remote_ep.port());
                }
                auto conn = std::make_shared<terila::network::Connection>(std::move(socket), data, mobs,
                    srv.view_distance, srv.simulation_distance,
                    static_cast<int>(srv.max_players), next_player_entity_id++);
                conn->start();
                players.push_back(conn);
                self(self);
            }
        );
        };

    do_accept(do_accept);

    asio::steady_timer tick_timer(io);
    std::function<void()> schedule_tick = [&]() {
        tick_timer.expires_after(std::chrono::milliseconds(50));
        tick_timer.async_wait([&](const asio::error_code& error) {
            if (error) {
                return;
            }

            std::vector<terila::entity::PlayerPos> player_positions;
            player_positions.reserve(players.size());
            for (const auto& player : players) {
                if (const auto conn = player.lock()) {
                    if (conn->joined()) {
                        player_positions.push_back(conn->player_position());
                    }
                }
            }

            mobs.tick(player_positions);

            for (auto it = players.begin(); it != players.end();) {
                if (const auto conn = it->lock()) {
                    conn->tick_entities();
                    ++it;
                }
                else {
                    it = players.erase(it);
                }
            }

            schedule_tick();
        });
        };

    // Without a handler, Ctrl+C kills the process before the log and the
    // connections get a chance to shut down.
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const asio::error_code& error, int) {
        if (error) {
            return;
        }
        LOG_INFO(Network, "shutdown requested, stopping");
        io.stop();
    });

    schedule_tick();
    io.run();

    terila::log::shutdown();
    return 0;
}