#pragma once

#include "commands/selector.hpp"
#include "item/components.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace terila::commands {

    // The world clock a /time command drives, as callbacks so the command module never
    // includes the world.
    struct TimeApi {
        std::function<std::int64_t()> game_time;
        std::function<std::int64_t()> time_of_day;
        std::function<bool()> night;
        std::function<void(std::int64_t)> set;
        std::function<void(std::int64_t)> add;
        // Tells every player about the new time instead of waiting for the next sync.
        std::function<void()> sync;
    };

    // What a command may ask of the game. The connection fills this in, so the commands
    // know nothing about sockets, inventories, players or the clock.
    struct Context {
        Target self;
        std::function<std::vector<Target>()> players;
        std::function<void(std::string_view)> reply;
        std::function<bool(std::int32_t entity, std::int32_t item_id, std::int32_t count,
            const item::ComponentsPtr&)> give;
        // Runs a command line as this player, and as another player for /execute as.
        std::function<void(std::string_view)> run;
        std::function<bool(std::int32_t entity, std::string_view)> run_as;
        const std::unordered_map<std::string, std::int32_t>* item_ids = nullptr;
        const item::ComponentIds* component_ids = nullptr;
        const TimeApi* time = nullptr;
    };

} // namespace terila::commands
