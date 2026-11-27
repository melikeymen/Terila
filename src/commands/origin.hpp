#pragma once

#include "commands/arguments/selector.hpp"
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
        // Everything the source can see, players included. A selector written as @e reads
        // this; @a, @p, @r and a plain name stay on players().
        virtual std::vector<Target> entities() const { return players(); }
        virtual bool give(std::int32_t entity, std::int32_t item_id, std::int32_t count,
            const item::ComponentsPtr& components) = 0;
        virtual bool teleport(std::int32_t entity, double x, double y, double z,
            float yaw, float pitch) = 0;
        // Moves an entity into another dimension. False when the build has no such
        // dimension, or when the entity is already in it: the caller then falls back to a
        // plain teleport, which is the same move inside one world.
        virtual bool teleport_in(std::int32_t entity, std::string_view dimension, double x,
            double y, double z, float yaw, float pitch) {
            (void)entity;
            (void)dimension;
            (void)x;
            (void)y;
            (void)z;
            (void)yaw;
            (void)pitch;
            return false;
        }
        virtual bool set_gamemode(std::int32_t entity, int mode) = 0;
        virtual int gamemode(std::int32_t entity) const = 0;
        // False when the type has no entity behind it in this build.
        virtual bool summon(std::string_view type, double x, double y, double z) = 0;
        virtual bool summonable(std::string_view type) const = 0;
        // One line to one player, wherever that player is connected.
        virtual bool tell(std::int32_t entity, std::string_view text) = 0;
        // One line to everyone, the caller included. Returns how many received it.
        virtual std::size_t broadcast(std::string_view text) {
            (void)text;
            return 0;
        }
        // A hit that ignores invulnerability, which is what /kill asks for: a creative
        // player dies to it the same way a survival one does. False when the target is
        // already gone.
        virtual bool kill(std::int32_t entity, std::string_view cause) {
            (void)entity;
            (void)cause;
            return false;
        }
        // The world seed and the player limit /list reports.
        virtual std::int64_t seed() const { return 0; }
        virtual int max_players() const { return 0; }

        // A hit that follows the damage rules, which is what /damage asks for: armour, the
        // game mode and invulnerability all still apply.
        virtual bool damage(std::int32_t entity, float amount, std::string_view cause) {
            (void)entity;
            (void)amount;
            (void)cause;
            return false;
        }

        // The world difficulty as the protocol numbers it (0 peaceful .. 3 hard).
        virtual int difficulty() const { return 2; }
        virtual bool set_difficulty(int level) {
            (void)level;
            return false;
        }

        // The game mode a player joining the world starts in.
        virtual int default_gamemode() const { return 0; }
        virtual bool set_default_gamemode(int mode) {
            (void)mode;
            return false;
        }

        // One block, for the commands that edit the world. A negative state reads as
        // "outside the world".
        virtual std::int32_t block_state_at(std::int32_t x, std::int32_t y,
            std::int32_t z) const {
            (void)x;
            (void)y;
            (void)z;
            return -1;
        }
        virtual bool set_block_state(std::int32_t x, std::int32_t y, std::int32_t z,
            std::int32_t state) {
            (void)x;
            (void)y;
            (void)z;
            (void)state;
            return false;
        }

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
        // Set by `execute in <dimension>`: empty means the dimension the source stands in.
        std::string dimension;
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
