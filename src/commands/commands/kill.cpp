#include "commands/registry.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        // Large enough to empty any health bar the game hands out, so the hit never has to
        // guess what the target's maximum is.
        constexpr float kFatalHit = 1000.0f;

        Result run_kill(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            std::vector<Target> victims;
            if (const auto* selector = exec.args.get<Selector>("targets")) {
                std::string error;
                victims = targets_of(*selector, origin, error);
                if (victims.empty()) {
                    origin.say(error.empty() ? "No entity was found" : error);
                    return Result{ 0, false };
                }
            }
            else {
                if (origin.console) {
                    origin.say("A player is required to run this command");
                    return Result{ 0, false };
                }
                victims.push_back(Target{ origin.entity, origin.name, origin.x, origin.y,
                    origin.z });
            }

            std::int32_t killed = 0;
            for (const auto& victim : victims) {
                if (!origin.game->kill(victim.entity, "the kill command")) {
                    continue;
                }
                ++killed;
            }

            if (killed == 0) {
                origin.say("Nothing was killed");
                return Result{ 0, false };
            }
            if (killed == 1) {
                origin.say("Killed " + victims.front().name);
            }
            else {
                origin.say(std::format("Killed {} entities", killed));
            }
            return Result{ killed, true };
        }

    } // namespace

    void install_kill(Registry& registry) {
        Node& kill = registry.root().literal("kill", Capability::Operator);
        kill.executes(run_kill);
        kill.argument("targets", Parser::entities(), Capability::Operator).executes(run_kill);
    }

} // namespace terila::commands
