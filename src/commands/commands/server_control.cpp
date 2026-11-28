#include "commands/registry.hpp"

#include <format>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        Result run_stop(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr || !origin.game->stop_server()) {
                origin.say("The server could not be stopped");
                return Result{ 0, false };
            }
            origin.say("Stopping the server");
            return Result{ 1, true };
        }

        Result run_save_all(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr || !origin.game->save_all()) {
                origin.say("The world could not be saved");
                return Result{ 0, false };
            }
            origin.say("Saved the game");
            return Result{ 1, true };
        }

        Result run_kick(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            const auto* selector = exec.args.get<Selector>("targets");
            if (selector == nullptr) {
                return Result{ 0, false };
            }

            std::string reason = "Kicked by an operator";
            if (const auto* text = exec.args.get<std::string>("reason")) {
                reason = *text;
            }

            std::string error;
            const auto targets = targets_of(*selector, origin, error);
            if (targets.empty()) {
                origin.say(error.empty() ? "No player was found" : error);
                return Result{ 0, false };
            }

            std::int32_t kicked = 0;
            for (const auto& target : targets) {
                if (!origin.game->kick(target.entity, reason)) {
                    continue;
                }
                ++kicked;
            }
            if (kicked == 0) {
                origin.say("No player was kicked");
                return Result{ 0, false };
            }
            origin.say(std::format("Kicked {} player{}: {}", kicked,
                kicked == 1 ? "" : "s", reason));
            return Result{ kicked, true };
        }

    } // namespace

    void install_server_control(Registry& registry) {
        registry.root().literal("stop", Capability::Owner).executes(run_stop);

        // The write is already done when the call returns, so the `flush` spelling of the
        // command asks for nothing the plain one leaves out; the literal stays because that
        // is what a player types.
        Node& save_all = registry.root().literal("save-all", Capability::Owner);
        save_all.executes(run_save_all);
        save_all.literal("flush").executes(run_save_all);

        Node& kick = registry.root().literal("kick", Capability::Operator);
        Node& targets = kick.argument("targets", Parser::players());
        targets.executes(run_kick);
        targets.argument("reason", Parser::greedy()).executes(run_kick);
    }

} // namespace terila::commands
