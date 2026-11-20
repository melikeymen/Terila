#include <asio.hpp>
#include <csignal>
#ifdef _WIN32
#include <windows.h>
#endif
#include "log/logger.hpp"
#include "network/connection.hpp"
#include "data/recipe_book.hpp"
#include "data/registry_data.hpp"
#include "data/registry_loader.hpp"
#include "config/server_config.hpp"
#include "config/server_files.hpp"
#include "data/input_store.hpp"
#include "protocol/packet_ids_loader.hpp"
#include "entity/mob.hpp"
#include "entity/population.hpp"
#include "world/block_world.hpp"
#include "world/falling_blocks.hpp"
#include "world/fluid_flow.hpp"
#include "world/world_clock.hpp"
#include "world/world_generator.hpp"
#include "version/mc_version.hpp"
#include "world/trf/trf_store.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <optional>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <functional>
#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>

namespace fs = std::filesystem;

// The world directory lives next to the executable, so a build or a debug run never
// writes into the read-only input tree.
static fs::path executable_directory() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return fs::path(buffer).parent_path();
    }
#else
    std::error_code link_ec;
    const auto exe = fs::read_symlink("/proc/self/exe", link_ec);
    if (!link_ec) {
        return exe.parent_path();
    }
#endif
    return fs::current_path();
}

#ifndef TERILA_EMBEDDED_INPUT
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
#endif

// The EULA is an agreement, not a config value: a first run writes eula=false and the server
// keeps its console open, so typing the agreement there rewrites the file instead of leaving
// the operator with nothing but an exit code.
static bool accept_eula_on_console(const fs::path& eula_path) {
    std::fputs("eula.txt says eula=false. Type 'eula=true' and press Enter to accept the "
        "Minecraft EULA (https://aka.ms/MinecraftEULA), or 'stop' to quit.\n> ", stdout);
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::string answer = line;
        answer.erase(answer.begin(), std::find_if(answer.begin(), answer.end(),
            [](unsigned char c) { return !std::isspace(c); }));
        answer.erase(std::find_if(answer.rbegin(), answer.rend(),
            [](unsigned char c) { return !std::isspace(c); }).base(), answer.end());
        for (char& c : answer) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        if (answer == "stop" || answer == "quit" || answer == "exit") {
            return false;
        }
        if (answer == "eula=true" || answer == "eula true" || answer == "eula=accept" ||
            answer == "eula accept" || answer == "accept" || answer == "yes") {
            if (!terila::config::write_eula_accepted(eula_path)) {
                std::fputs("cannot write eula.txt, the agreement was not saved\n", stderr);
                return false;
            }
            std::fputs("eula.txt now says eula=true\n", stdout);
            return true;
        }

        std::fputs("Type 'eula=true' to accept the Minecraft EULA or 'stop' to quit.\n> ",
            stdout);
        std::fflush(stdout);
    }
    // A console that closed without an answer is not an agreement.
    return false;
}

int main() {
#ifdef TERILA_EMBEDDED_INPUT
    // The data travels in the binary, so there is no input/ directory to find; the executable's
    // own directory anchors server.properties, eula.txt and the world.
    const fs::path base = executable_directory();
#else
    const auto base = find_input_dir();
    if (base.empty()) {
        const std::string cwd = fs::current_path().string();
        std::fprintf(stderr, "input/ not found. cwd=%s\n", cwd.c_str());
        return 7;
    }
#endif

    // A first run has neither file. Write both instead of failing, and read eula.txt back:
    // eula=false keeps the world closed until the console accepts the agreement.
    const fs::path properties_path = base / "server.properties";
    const fs::path eula_path = base / "eula.txt";
    bool properties_created = false;
    bool eula_created = false;
    const bool config_files_written =
        terila::config::ensure_default_file(properties_path,
            terila::config::kDefaultProperties, &properties_created) &&
        terila::config::ensure_default_file(eula_path, terila::config::kDefaultEula,
            &eula_created);
    if (!config_files_written) {
        std::fprintf(stderr, "cannot write server.properties or eula.txt next to %s\n",
            base.string().c_str());
        return 7;
    }
    bool eula_ok = terila::config::eula_accepted(eula_path);

    terila::config::ServerConfig srv;
    terila::config::load(srv, properties_path.string());

    terila::log::Config cfg;
    if (auto lvl = terila::log::parse_level(srv.log_level)) cfg.min_level = *lvl;
    cfg.file_path = srv.log_file;
    if (srv.log_exit_level == "never") {
        cfg.exit_enabled = false;
    }
    else if (auto exit_level = terila::log::parse_level(srv.log_exit_level)) {
        cfg.exit_on_level = *exit_level;
    }
    terila::log::init(cfg);

    LOG_INFO(Main, "server starting");
    if (properties_created) {
        LOG_INFO(Config, "server.properties was missing, wrote the defaults to {}",
            properties_path.string());
    }
    if (eula_created) {
        LOG_INFO(Config, "eula.txt was missing, wrote it with eula=false ({})",
            eula_path.string());
    }
    if (!eula_ok) {
        // The server stays up and asks, so the first run ends with a world instead of a wall.
        if (!accept_eula_on_console(eula_path)) {
            LOG_INFO(Config, "stopped without accepting the Minecraft EULA");
            terila::log::shutdown();
            return 0;
        }
        eula_ok = true;
        LOG_INFO(Config, "EULA accepted, starting the world");
    }
    LOG_DEBUG(Main, "config: bind={} port={} log_level={} log_exit_level={} online_mode={} network_encryption={} view_distance={} simulation_distance={} level_type={}",
        srv.bind_address, srv.port, srv.log_level, srv.log_exit_level, srv.online_mode,
        srv.network_encryption, srv.view_distance, srv.simulation_distance, srv.level_type);
    if (srv.log_exit_level != "never" && !terila::log::parse_level(srv.log_exit_level)) {
        LOG_WARN(Config, "unknown log-exit-level '{}', keeping fatal", srv.log_exit_level);
    }

    // Both settings are advertised as working; silently running without them is a whole
    // subsystem missing, so they are CRITICAL rather than a warning.
    if (srv.online_mode) {
        LOG_CRITICAL(Main, "online-mode=true but session auth is not implemented; falling back "
            "to offline mode — any client can claim any username");
        srv.online_mode = false;
    }
    if (srv.network_encryption) {
        LOG_CRITICAL(Main, "network-encryption=true but AES/CFB8 is not implemented; traffic "
            "stays plaintext — do not expose this server");
        srv.network_encryption = false;
    }

    // level-type picks which generator the server streams.
    terila::world::WorldType world_type = terila::world::WorldType::Terrain;
    if (const auto parsed = terila::world::parse_world_type(srv.level_type)) {
        world_type = *parsed;
    }
    else {
        LOG_WARN(Main, "unknown level-type '{}', using normal", srv.level_type);
    }

#ifndef TERILA_EMBEDDED_INPUT
    // A build that carries its data has no directory to point at; everything goes through the
    // input store from here on.
    terila::data::input::mount((base / "input").string());
#endif
    LOG_INFO(Main, "input data: {}", terila::data::input::describe());

    if (!terila::protocol::load_packet_ids("packets/ids.json")) {
        LOG_FATAL(Protocol, "cannot load packet ids: packets/ids.json");
        return 7;
    }

    // The ids and every payload layout below them belong to one release. A dump from
    // another protocol would resolve to the wrong numbers, so it is refused instead of
    // starting a server that answers with packets its clients cannot read.
    if (const auto& source = terila::protocol::packet_id_source();
        !terila::version::matches_source(source.protocol)) {
        LOG_FATAL(Main, "input/ was extracted from {} (protocol {}), this build speaks {}",
            source.version.empty() ? "another release" : source.version, source.protocol,
            terila::version::label());
        return 7;
    }

    LOG_INFO(Main, "speaking Minecraft {}", terila::version::label());

    terila::data::RegistryData data;

    // Registry NBT payloads. The store lists them, so the same walk works against the
    // generated table and against a directory.
    static const std::string kNbtPrefix = "registries/nbt/";
    std::size_t registry_payloads = 0;
    for (const auto& file : terila::data::input::files_under(kNbtPrefix)) {
        if (file.size() <= kNbtPrefix.size() + 4 ||
            file.compare(file.size() - 4, 4, ".nbt") != 0) {
            continue;
        }
        std::string id = file.substr(kNbtPrefix.size(), file.size() - kNbtPrefix.size() - 4);
        const auto slash = id.find('/');
        if (slash != std::string::npos) id[slash] = ':';
        if (terila::data::load_binary(data.nbt_payloads[id], file)) {
            ++registry_payloads;
        }
    }
    if (registry_payloads == 0) {
        LOG_FATAL(Registry, "no registry payloads under {}", kNbtPrefix);
        return 7;
    }

    terila::data::load_binary(data.tag_payload, "tags/client_tags.bin");

    // Datapack registry filter
    {
        std::string index_text;
        if (!terila::data::input::read_text("registries/index.json", index_text)) {
            LOG_FATAL(Registry, "index.json not found in the input data");
            return 7;
        }
        auto idx = nlohmann::json::parse(index_text);

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

    terila::data::load_binary(data.recipe_payload, "recipes/minecraft.nbt");
    terila::data::load_binary(data.loot_payload, "loot/minecraft.nbt");
    terila::data::load_binary(data.advancement_payload, "advancements/minecraft.nbt");
    terila::data::load_command_tree(data.command_tree_payload, "commands/tree.nbt");

    terila::data::load_blocks(data, "blocks/state_ids.json");
    // After the state ids: breaking is answered per state, not per block kind.
    terila::data::load_block_break_info(data, "blocks/minecraft.json");
    // The other half of "how long does this dig take": which tags a block is in, and how
    // fast each tool item works.
    terila::data::load_block_tags(data, "tags/blocks.json");
    terila::data::load_tool_rules(data, "items/minecraft.json");
    terila::data::load_biome_ids(data.biome_ids, "worldgen/biomes.json");
    terila::data::load_dimensions(data.overworld, "worldgen/dimensions.json");
    terila::data::load_entity_type_ids(data.entity_type_ids, "entities/minecraft.json");
    terila::data::load_entity_categories(data.entity_categories, "entities/minecraft.json");
    terila::data::load_item_ids(data.item_ids, "items/minecraft.json");
    terila::data::load_data_component_ids(data.component_ids,
        "registries/dump/minecraft/data_component_type.json");
    terila::data::load_command_argument_types(data.command_argument_types,
        "registries/dump/minecraft/command_argument_type.json");
    terila::data::load_enchantment_ids(data.enchantment_ids,
        "registries/dump/minecraft/enchantment.json");

    if (data.block_state_ids.empty()) { LOG_FATAL(Data, "block_state_ids empty"); return 7; }
    if (data.biome_ids.empty()) { LOG_FATAL(Data, "biome_ids empty");       return 7; }
    if (data.entity_type_ids.empty()) {
        LOG_CRITICAL(Data, "entity_type_ids is empty — the mob subsystem is disabled");
    }

    LOG_DEBUG(Registry, "datapack registries: {}", data.datapack_registries.size());
    LOG_DEBUG(Registry, "registry payloads: {}", data.nbt_payloads.size());

    // One terrain generator decides where the ground is, so mobs stand on the surface
    // instead of the old flat layer.
    terila::world::WorldGenerator world_terrain;
    const auto resolve_block_id = [&data](std::string_view name) -> std::optional<std::int32_t> {
        const auto it = data.block_state_ids.find(std::string(name));
        return it == data.block_state_ids.end() ? std::nullopt
            : std::optional<std::int32_t>(it->second);
    };
    const auto resolve_biome_id = [&data](std::string_view name) -> std::optional<std::int32_t> {
        const auto it = data.biome_ids.find(std::string(name));
        return it == data.biome_ids.end() ? std::nullopt
            : std::optional<std::int32_t>(it->second);
    };
    // The seed belongs to the world: an explicit level-seed gets its own folder (so naming
    // another seed starts another world instead of streaming the old one), while a random
    // world keeps one folder and takes the seed drawn into its descriptor on first run.
    const auto explicit_seed = srv.random_seed()
        ? std::optional<std::uint32_t>{}
        : std::optional<std::uint32_t>{ srv.seed_value() };
    std::string world_name{ terila::world::world_type_name(world_type) };
    if (explicit_seed) {
        world_name += "-" + std::to_string(*explicit_seed);
    }
    const fs::path world_root = executable_directory() / "world" / world_name;
    auto world_identity = terila::world::trf::load_or_create_world(world_root, explicit_seed);
    if (!world_identity) {
        LOG_FATAL(Main, "cannot prepare the world directory '{}': {}", world_root.string(),
            world_identity.error().detail);
        return 7;
    }
    const auto world_seed = static_cast<std::uint32_t>(world_identity->seed);
    if (!srv.random_seed() && world_seed != srv.seed_value()) {
        LOG_WARN(Main, "world keeps its own seed {}; level-seed {} applies to new worlds",
            world_seed, srv.seed_value());
    }

    if (!world_terrain.init(world_type, data.overworld.min_y, data.overworld.height / 16,
            world_seed, resolve_block_id, resolve_biome_id)) {
        LOG_FATAL(Data, "{} world generator failed to initialise",
            terila::world::world_type_name(world_type));
        return 7;
    }

    // The spawn column is searched, not assumed: a fixed (8, 8) is whatever the seed puts
    // there, which on a mountainous seed is a peak the player has to jump down from.
    const auto spawn = world_terrain.spawn_point();
    const int spawn_surface = spawn.surface_y;
    LOG_INFO(Chunk, "{} world ready: seed {} (level-seed '{}'), spawn at ({}, {}) stands on y={}, sea level {}",
        terila::world::world_type_name(world_type), world_terrain.seed(),
        srv.random_seed() ? std::string("random") : srv.level_seed,
        spawn.x, spawn.z, spawn_surface, world_terrain.sea_level());

    terila::entity::MobManager mobs;
    // Mobs follow the terrain: every column reports its own surface, so nobody spawns in
    // the air or inside a hill and wandering herds walk over the landscape.
    mobs.set_ground(static_cast<double>(spawn_surface + 1));
    mobs.set_ground_resolver([&world_terrain](double x, double z) {
        return static_cast<double>(world_terrain.surface_height(
            static_cast<std::int32_t>(std::floor(x)),
            static_cast<std::int32_t>(std::floor(z))) + 1.0);
        });
    // No leash: a mob walks where its goals send it, and the natural spawner fills the
    // world around the players, so the population grows with the explored area.

    // The population a world starts with comes from the biome's own spawn list: weights,
    // pack sizes and the per-category caps are all in input/worldgen/biomes.json.
    mobs.set_entity_type_ids(&data.entity_type_ids);

    const auto configured_difficulty = terila::entity::difficulty_from(srv.difficulty);
    const auto world_difficulty = configured_difficulty.value_or(
        terila::entity::Difficulty::Normal);
    if (!configured_difficulty) {
        LOG_WARN(Config, "unknown difficulty '{}', using normal", srv.difficulty);
    }

    terila::entity::Population spawner;
    terila::entity::SpawnWorld spawn_world;
    spawn_world.biome_index = [&world_terrain](std::int32_t x, std::int32_t z) {
        return world_terrain.biome_at(x, z);
        };
    spawn_world.surface_y = [&world_terrain](std::int32_t x, std::int32_t z) {
        return world_terrain.surface_height(x, z) + 1;
        };
    spawn_world.is_water = [&world_terrain](std::int32_t x, std::int32_t z) {
        return world_terrain.surface_height(x, z) + 1 <= world_terrain.sea_level();
        };
    spawn_world.block_light = [](std::int32_t, std::int32_t, std::int32_t) { return 0; };
    spawn_world.sky_light = [&world_terrain](std::int32_t x, std::int32_t y, std::int32_t z) {
        // Everything above the column's sky top sees the sky; underneath it is roofed.
        return y > world_terrain.sky_top(x, z) ? 15 : 0;
        };
    spawn_world.sea_level = world_terrain.sea_level();
    // SpawnPlacements asks three things of a position: is it empty, what is below it, and is
    // that surface the animals tag. The world generator answers with one classification.
    spawn_world.block_kind = [&world_terrain](std::int32_t x, std::int32_t y,
        std::int32_t z) { return world_terrain.block_kind_at(x, y, z); };
    spawner.set_world(spawn_world);
    spawner.set_spawn_point(spawn.x, spawn.z);
    spawner.set_difficulty(world_difficulty);

    // Undead keep out of the sun: a column whose sky top is above the mob is shaded.
    mobs.set_shade_resolver([&world_terrain](double x, double y, double z) {
        return y < static_cast<double>(world_terrain.sky_top(
            static_cast<std::int32_t>(std::floor(x)),
            static_cast<std::int32_t>(std::floor(z))));
        });

    // Mobs that swim in the biome's water entries need to know when they are in it.
    mobs.set_water_resolver([&world_terrain](double x, double z) {
        return world_terrain.surface_height(
            static_cast<std::int32_t>(std::floor(x)),
            static_cast<std::int32_t>(std::floor(z))) + 1 <= world_terrain.sea_level();
        });

    // The world clock: twenty ticks a second, 24000 ticks a day. It drives the sky the
    // clients see and the monster rule of the natural spawner.
    auto clock = std::make_shared<terila::world::WorldClock>();
    clock->set_time_of_day(terila::world::WorldClock::kNoon);

    std::uint32_t population_rng = static_cast<std::uint32_t>(world_terrain.seed()) ^ 0x5EED5EEDu;
    spawner.set_moon_brightness(clock->sky_darken());
    const std::size_t spawned = spawner.initial(mobs, spawn.x, spawn.z, 96.0,
        population_rng, clock->night());
    LOG_INFO(Entity, "spawned {} mobs around ({}, {}), each from the spawn list of its own biome",
        spawned, spawn.x, spawn.z);
    if (spawned == 0) {
        LOG_WARN(Entity, "the biome spawn list produced nothing — check the entity registry");
    }

    // Items whose id doubles as a placeable block, e.g. minecraft:stone.
    std::unordered_map<std::int32_t, std::int32_t> item_block_state;
    for (const auto& entry : data.item_ids) {
        const auto block = data.block_state_ids.find(entry.first);
        if (block == data.block_state_ids.end()) continue;
        item_block_state[entry.second] = block->second;
    }
    LOG_DEBUG(Data, "placeable block items: {}", item_block_state.size());

    // The reverse of the table above: which item a block state turns into when it is
    // broken or picked with the middle mouse button.
    std::unordered_map<std::int32_t, std::int32_t> block_state_item;
    for (const auto& entry : item_block_state) {
        if (block_state_item.find(entry.second) == block_state_item.end()) {
            block_state_item[entry.second] = entry.first;
        }
    }

    std::int32_t item_entity_type = 0;
    if (const auto it = data.entity_type_ids.find("minecraft:item");
        it != data.entity_type_ids.end()) {
        item_entity_type = it->second;
    }
    else {
        LOG_CRITICAL(Entity, "minecraft:item is missing from the entity registry — block drops "
            "and the pickup chain are disabled");
    }

    terila::entity::ItemDropManager drops;

    terila::data::RecipeBook recipes;
    if (!recipes.load("recipes/minecraft.json", "tags/items.json",
            data.item_ids, data.block_state_ids)) {
        LOG_CRITICAL(Data, "recipes failed to load — crafting tables and furnaces stay closed");
    }

    // Blocks that open a window when used: 1 = crafting table, 2 = furnace. Every furnace
    // variant opens the menu, not only the default state.
    std::unordered_map<std::int32_t, std::int32_t> menu_blocks;
    for (const auto& entry : data.block_state_ids) {
        if (entry.first.rfind("minecraft:crafting_table", 0) == 0) {
            menu_blocks[entry.second] = 1;
        }
    }
    for (const auto& entry : data.block_states) {
        if (entry.first.rfind("minecraft:furnace", 0) != 0) continue;
        for (const auto& variant : entry.second) {
            menu_blocks[variant.id] = 2;
        }
    }

    // Furnace variants indexed [facing * 2 + lit]: 0 north, 1 south, 2 west, 3 east.
    std::array<std::int32_t, 8> furnace_states{};
    furnace_states.fill(-1);
    if (const auto furnace = data.block_states.find("minecraft:furnace");
        furnace != data.block_states.end()) {
        static const std::unordered_map<std::string, int> kFacing{
            { "north", 0 }, { "south", 1 }, { "west", 2 }, { "east", 3 }
        };
        for (const auto& variant : furnace->second) {
            const auto facing = variant.properties.find("facing");
            const auto lit = variant.properties.find("lit");
            if (facing == variant.properties.end() || lit == variant.properties.end()) continue;
            const auto index = kFacing.find(facing->second);
            if (index == kFacing.end()) continue;
            furnace_states[static_cast<std::size_t>(index->second * 2 +
                (lit->second == "true" ? 1 : 0))] = variant.id;
        }
    }
    const auto furnace_item = data.item_ids.find("minecraft:furnace");
    const std::int32_t furnace_item_id =
        furnace_item == data.item_ids.end() ? 0 : furnace_item->second;

    // The bucket trio: pouring a fluid or scooping a source turns one into the other.
    terila::network::FluidItems fluid_items;
    for (const auto& entry : data.item_ids) {
        if (entry.first == "minecraft:bucket") fluid_items.bucket = entry.second;
        else if (entry.first == "minecraft:water_bucket") fluid_items.water_bucket = entry.second;
        else if (entry.first == "minecraft:lava_bucket") fluid_items.lava_bucket = entry.second;
    }
    LOG_DEBUG(Data, "blocks with a window: {}, furnace variants: {}", menu_blocks.size(),
        std::count_if(furnace_states.begin(), furnace_states.end(),
            [](std::int32_t state) { return state >= 0; }));

    terila::world::BlockWorld world;

    // Block physics: sand, gravel and the concrete powders are not stable. The states that
    // fall are the vanilla FallingBlock kinds, and the states they fall through are the
    // game's own #minecraft:replaceable tag (air, water, lava, plants, fire, snow layers),
    // so neither list is guessed from a hard-coded set of ids.
    terila::world::FallingBlocks falling;
    {
        std::unordered_set<std::int32_t> falling_states;
        std::unordered_set<std::int32_t> passes_through;
        for (const auto& [name, variants] : data.block_states) {
            const bool falls = terila::world::is_falling_block(name);
            const auto tags = data.block_tags.find(name);
            const bool replaceable = tags != data.block_tags.end() &&
                tags->second.count("minecraft:replaceable") != 0;
            if (!falls && !replaceable) {
                continue;
            }
            for (const auto& variant : variants) {
                if (falls) falling_states.insert(variant.id);
                if (replaceable) passes_through.insert(variant.id);
            }
        }
        const auto falling_count = falling_states.size();
        const auto through_count = passes_through.size();
        falling.set_world(&world, resolve_block_id("minecraft:air").value_or(0),
            data.overworld.min_y, data.overworld.min_y + data.overworld.height - 1);
        falling.set_states(std::move(falling_states), std::move(passes_through));
        falling.set_state_at([&world, &world_terrain](const terila::world::BlockPos& pos) {
            return world_terrain.state_at(pos.x, pos.y, pos.z);
            });
        LOG_DEBUG(Chunk, "block physics: {} falling states, {} states they fall through",
            falling_count, through_count);
    }

    // Fluid physics: water and lava each carry their own levels. The state ids come from the
    // registry, so both fluids and their sixteen levels match whatever the data dump
    // carries instead of a guessed offset.
    terila::world::FluidFlow fluids;
    {
        const auto load_levels = [&data](std::string_view name,
            terila::world::FluidFlow::Fluid& out) {
            const auto block = data.block_states.find(std::string(name));
            if (block == data.block_states.end()) {
                return false;
            }
            for (const auto& variant : block->second) {
                int value = 0;
                const auto level = variant.properties.find("level");
                if (level != variant.properties.end()) {
                    try {
                        value = std::stoi(level->second);
                    }
                    catch (...) {
                        continue;
                    }
                }
                if (value >= 0 && value < terila::world::FluidFlow::kLevels) {
                    out.states[static_cast<std::size_t>(value)] = variant.id;
                }
            }
            return out.states[0] >= 0;
        };

        terila::world::FluidFlow::Fluid water;
        if (!load_levels("minecraft:water", water)) {
            LOG_CRITICAL(Chunk, "minecraft:water is missing from the registry - water physics "
                "stays off");
        }
        water.drop_off = 1;
        water.slope_find = 4;
        water.tick_delay = 5;
        water.source_conversion = true;
        fluids.set_fluid(terila::world::FluidFlow::Kind::Water, water);

        terila::world::FluidFlow::Fluid lava;
        if (!load_levels("minecraft:lava", lava)) {
            LOG_CRITICAL(Chunk, "minecraft:lava is missing from the registry - lava physics "
                "stays off");
        }
        // Overworld lava: two levels per block, three blocks of reach, thirty ticks a step.
        lava.drop_off = 2;
        lava.slope_find = 2;
        lava.tick_delay = 30;
        lava.source_conversion = false;
        fluids.set_fluid(terila::world::FluidFlow::Kind::Lava, lava);

        std::unordered_set<std::int32_t> replaceable;
        for (const auto& [name, variants] : data.block_states) {
            const auto tags = data.block_tags.find(name);
            if (tags == data.block_tags.end() ||
                tags->second.count("minecraft:washed_away_by_fluids") == 0) {
                continue;
            }
            for (const auto& variant : variants) {
                replaceable.insert(variant.id);
            }
        }
        const auto replaceable_count = replaceable.size();
        fluids.set_world(&world, resolve_block_id("minecraft:air").value_or(0),
            data.overworld.min_y, data.overworld.min_y + data.overworld.height - 1);
        fluids.set_replaceable(std::move(replaceable));
        fluids.set_solids(resolve_block_id("minecraft:obsidian").value_or(-1),
            resolve_block_id("minecraft:cobblestone").value_or(-1),
            resolve_block_id("minecraft:stone").value_or(-1));
        fluids.set_state_at([&world_terrain](const terila::world::BlockPos& pos) {
            return world_terrain.state_at(pos.x, pos.y, pos.z);
            });
        LOG_DEBUG(Chunk, "fluid physics: water level 0 is state {}, lava level 0 is state {}, "
            "{} states can hold fluid", water.states[0], lava.states[0], replaceable_count);
    }

    // TRF region files live beside the executable, one folder per generator so a flat world
    // never streams chunks a terrain run saved. The seed lives in the descriptor inside.
    const auto region_dir = terila::world::trf::default_region_directory(world_root);
    terila::world::trf::ChunkStore chunk_store(region_dir, *world_identity);
    LOG_INFO(Main, "world directory: {}", world_root.string());
    LOG_DEBUG(Main, "region directory: {}", region_dir.string());

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

    // The simulation edits the overlay itself, so the tick loop collects the changed cells:
    // every player that holds the column is told at once, and the column is saved when the
    // cascade is over instead of once per moved cell.
    std::unordered_set<std::uint64_t> falling_dirty_chunks;
    int falling_dirty_ticks = 0;
    falling.set_change_sink([&](const terila::world::BlockPos& pos, std::int32_t state) {
        for (const auto& player : players) {
            if (const auto conn = player.lock()) {
                conn->send_world_block(pos, state);
            }
        }
        falling_dirty_chunks.insert(
            terila::world::BlockWorld::chunk_key(pos.x >> 4, pos.z >> 4));
        });

    fluids.set_change_sink([&](const terila::world::BlockPos& pos, std::int32_t state) {
        for (const auto& player : players) {
            if (const auto conn = player.lock()) {
                conn->send_world_block(pos, state);
            }
        }
        falling_dirty_chunks.insert(
            terila::world::BlockWorld::chunk_key(pos.x >> 4, pos.z >> 4));
        });

    // One registry for the whole server: connections register themselves when
    // they join, which is what lets players see each other and the status
    // response report a real online count.
    auto player_hub = std::make_shared<terila::network::PlayerHub>();
    player_hub->set_view_distance(srv.view_distance);
    if (const auto it = data.entity_type_ids.find("minecraft:player"); it != data.entity_type_ids.end()) {
        player_hub->set_player_type_id(it->second);
    }

    // Mobs use 1..N, so player entities start well above them.
    std::int32_t next_player_entity_id = 1000;

    auto do_accept = [&](auto&& self) -> void {
        acceptor.async_accept(
            [&acceptor, &data, &mobs, &world, &falling, &fluids, &item_block_state, &block_state_item, &drops, &item_entity_type, &recipes, &menu_blocks, &furnace_states, furnace_item_id, fluid_items, world_type, world_seed, &spawn, &srv, &players, &player_hub, &chunk_store, &next_player_entity_id, &world_terrain, clock, &world_difficulty, self](const asio::error_code& error, asio::ip::tcp::socket socket) {
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
                    world, falling, fluids, item_block_state, player_hub, &chunk_store, &drops, block_state_item,
                    item_entity_type, &recipes, menu_blocks, furnace_states, furnace_item_id,
                    fluid_items, world_seed, world_type, spawn.x, spawn.z, srv.view_distance,
                    srv.simulation_distance, static_cast<int>(srv.max_players),
                    next_player_entity_id++, srv.gamemode, static_cast<int>(world_difficulty),
                    srv.hardcore, clock, world_terrain.sea_level());
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

            // One game tick per loop pass, the same twenty a second the client expects.
            clock->tick();

            std::vector<terila::entity::PlayerPos> player_positions;
            player_positions.reserve(players.size());
            for (const auto& player : players) {
                if (const auto conn = player.lock()) {
                    // A dead player is not a target: the mobs lose interest until the
                    // respawn button puts them back in the world.
                    if (conn->joined() && !conn->dead()) {
                        player_positions.push_back(conn->player_position());
                    }
                }
            }

            // The cycle spawns what the biome allows, then the mobs themselves tick; the
            // difficulty decides what may live and what leaves.
            spawner.set_moon_brightness(clock->sky_darken());
            spawner.tick(mobs, player_positions, clock->night(), mobs.rng(), clock->game_time);
            mobs.tick(player_positions, world_difficulty, clock->night());

            // The mobs that landed a hit this tick: only the owning connection can take the
            // damage, so the hub routes it by entity id.
            if (player_hub) {
                for (const auto& hit : mobs.hits()) {
                    player_hub->damage_player(hit.player, static_cast<float>(hit.damage),
                        hit.attacker);
                }
            }

            drops.tick(player_positions);

            falling.tick();
            fluids.tick();
            if (!falling_dirty_chunks.empty()) {
                ++falling_dirty_ticks;
                // Re-encoding a column re-lights it and generates its neighbours, so the
                // save waits for the end of the cascade (or a second of continuous falling)
                // and then writes every touched column once.
                if ((falling.pending() == 0 && fluids.pending() == 0) ||
                    falling_dirty_ticks >= 20) {
                    for (const auto& player : players) {
                        if (const auto conn = player.lock()) {
                            for (const auto chunk : falling_dirty_chunks) {
                                conn->persist_chunk_edits(
                                    static_cast<std::int32_t>(chunk >> 32),
                                    static_cast<std::int32_t>(chunk & 0xFFFFFFFFu));
                            }
                            break;
                        }
                    }
                    falling_dirty_chunks.clear();
                    falling_dirty_ticks = 0;
                }
            }

            for (auto it = players.begin(); it != players.end();) {
                if (const auto conn = it->lock()) {
                    conn->tick_entities();
                    conn->tick_time();
                    // Region writes block the io thread for everyone, so a connection
                    // drains a handful per tick instead of a whole burst.
                    conn->flush_pending_saves(8);
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

    if (auto flushed = chunk_store.flush(); !flushed) {
        LOG_WARN(Main, "flushing region metadata failed: {}", flushed.error().detail);
    }

    terila::log::shutdown();
    return 0;
}
