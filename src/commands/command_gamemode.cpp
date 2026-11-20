#include "commands/registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        const char* mode_name(int mode) {
            switch (mode) {
            case 0: return "Survival Mode";
            case 1: return "Creative Mode";
            case 2: return "Adventure Mode";
            case 3: return "Spectator Mode";
            default: return "Unknown Mode";
            }
        }

        // /gamemode <mode> [targets]. The mode is a number by the time it reaches here; the
        // parser turned the names into the ids the protocol uses.
        Result run_gamemode(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }
            const auto* mode_value = exec.args.get<std::int64_t>("gamemode");
            if (mode_value == nullptr) {
                return Result{ 0, false };
            }
            const int mode = static_cast<int>(*mode_value);

            std::vector<Target> targets;
            const auto* selector = exec.args.get<Selector>("target");
            if (selector != nullptr) {
                std::string error;
                targets = targets_of(*selector, origin, error);
                if (targets.empty()) {
                    origin.say(error.empty() ? "No player matched" : error);
                    return Result{ 0, false };
                }
            }
            else {
                if (origin.console) {
                    origin.say("A player is required to change the game mode of");
                    return Result{ 0, false };
                }
                targets.push_back(Target{ origin.entity, origin.name, origin.x, origin.y,
                    origin.z });
            }

            std::int32_t changed = 0;
            for (const auto& target : targets) {
                if (!origin.game->set_gamemode(target.entity, mode)) {
                    continue;
                }
                ++changed;
                if (target.entity == origin.entity) {
                    origin.say(std::string("Set own game mode to ") + mode_name(mode));
                }
                else {
                    origin.say("Set " + target.name + "'s game mode to " + mode_name(mode));
                }
            }

            if (changed == 0) {
                origin.say("The game mode was already " + std::string(mode_name(mode)));
                return Result{ 0, false };
            }
            return Result{ changed, true };
        }

    } // namespace

    void install_gamemode(Registry& registry) {
        Node& gamemode = registry.root().literal("gamemode");
        Node& mode = gamemode.argument("gamemode", Parser::gamemode());
        mode.executes(run_gamemode);
        mode.argument("target", Parser::players()).executes(run_gamemode);
    }

} // namespace terila::commands
