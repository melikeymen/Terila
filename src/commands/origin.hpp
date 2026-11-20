#pragma once

#include "commands/selector.hpp"
#include "item/components.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace terila::commands {

    // What a node asks of the source. Levels are ordered, so a node that needs Operator is
    // also runnable by Owner without a second flag.
    enum class Capability : std::uint8_t {
        Everyone = 0,
        Helper = 1,
        Operator = 2,
        Owner = 3
    };

    // What a command may do to the game. The network layer implements it; a command module
    // knows nothing about sockets, inventories or the mob container.
    struct GameApi {
        virtual ~GameApi() = default;

        virtual std::vector<Target> players() const = 0;
        virtual bool give(std::int32_t entity, std::int32_t item_id, std::int32_t count,
            const item::ComponentsPtr& components) = 0;
        virtual bool teleport(std::int32_t entity, double x, double y, double z,
            float yaw, float pitch) = 0;
        virtual bool set_gamemode(std::int32_t entity, int mode) = 0;
        virtual int gamemode(std::int32_t entity) const = 0;
        // False when the type has no entity behind it in this build.
        virtual bool summon(std::string_view type, double x, double y, double z) = 0;
        virtual bool summonable(std::string_view type) const = 0;
        // One line to one player, wherever that player is connected.
        virtual bool tell(std::int32_t entity, std::string_view text) = 0;

        virtual std::int64_t game_time() const = 0;
        virtual std::int64_t time_of_day() const = 0;
        virtual bool night() const = 0;
        virtual void set_time(std::int64_t ticks) = 0;
        virtual void add_time(std::int64_t ticks) = 0;
        virtual void sync_time() = 0;
    };

    // Who runs a command and from where. A console origin has no entity and no position it
    // can act from; commands that need one say so instead of guessing.
    struct Origin {
        std::int32_t entity = 0;
        std::string name;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        float yaw = 0.0f;
        float pitch = 0.0f;
        Capability capability = Capability::Owner;
        bool console = false;
        bool silent = false;
        std::function<void(std::string_view)> reply;
        GameApi* game = nullptr;

        void say(std::string_view text) const {
            if (!silent && reply) {
                reply(text);
            }
        }
    };

} // namespace terila::commands
