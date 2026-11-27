#include "commands/registry.hpp"
#include "entity/difficulty.hpp"

#include <cstdint>
#include <format>
#include <string>

namespace terila::commands {

    namespace {

        constexpr const char* kModeNames[] = { "survival", "creative", "adventure",
            "spectator" };

        std::string difficulty_word(int level) {
            if (level < 0 || level > 3) {
                return "unknown";
            }
            return terila::entity::difficulty_name(
                static_cast<terila::entity::Difficulty>(level));
        }

        Result query_difficulty(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }
            const int level = origin.game->difficulty();
            origin.say(std::format("The difficulty is {}", difficulty_word(level)));
            return Result{ level, true };
        }

        Result apply_difficulty(Exec& exec, int level) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }
            if (origin.game->difficulty() == level) {
                origin.say(std::format("The difficulty is already {}", difficulty_word(level)));
                return Result{ 0, false };
            }
            if (!origin.game->set_difficulty(level)) {
                origin.say("The difficulty could not be changed");
                return Result{ 0, false };
            }
            origin.say(std::format("The difficulty has been set to {}", difficulty_word(level)));
            return Result{ level, true };
        }

        Result run_default_gamemode(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }
            const auto* mode = exec.args.get<std::int64_t>("gamemode");
            if (mode == nullptr || *mode < 0 || *mode > 3) {
                return Result{ 0, false };
            }
            if (origin.game->default_gamemode() == static_cast<int>(*mode)) {
                origin.say(std::format("The default game mode is already {}",
                    kModeNames[*mode]));
                return Result{ 0, false };
            }
            if (!origin.game->set_default_gamemode(static_cast<int>(*mode))) {
                origin.say("The default game mode could not be changed");
                return Result{ 0, false };
            }
            origin.say(std::format("The default game mode is now {}", kModeNames[*mode]));
            return Result{ static_cast<std::int32_t>(*mode), true };
        }

    } // namespace

    void install_world_state(Registry& registry) {
        Node& difficulty = registry.root().literal("difficulty", Capability::Operator);
        difficulty.executes(query_difficulty);
        const std::pair<const char*, int> levels[] = {
            { "peaceful", 0 }, { "easy", 1 }, { "normal", 2 }, { "hard", 3 },
        };
        for (const auto& [name, level] : levels) {
            difficulty.literal(name).executes([level](Exec& exec) {
                return apply_difficulty(exec, level);
            });
        }

        Node& default_gamemode =
            registry.root().literal("defaultgamemode", Capability::Operator);
        default_gamemode.argument("gamemode", Parser::gamemode()).executes(run_default_gamemode);
    }

} // namespace terila::commands
