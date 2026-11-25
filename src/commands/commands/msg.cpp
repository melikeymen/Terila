#include "commands/registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        Result run_message(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            const auto* selector = exec.args.get<Selector>("targets");
            const auto* message = exec.args.get<std::string>("message");
            if (selector == nullptr || message == nullptr) {
                return Result{ 0, false };
            }

            std::string error;
            const auto targets = targets_of(*selector, origin, error);
            if (targets.empty()) {
                origin.say(error.empty() ? "No player matched" : error);
                return Result{ 0, false };
            }

            const std::string sender = origin.console ? "Server" : origin.name;
            std::int32_t sent = 0;
            for (const auto& target : targets) {
                if (!origin.game->tell(target.entity,
                        sender + " whispers to you: " + *message)) {
                    continue;
                }
                ++sent;
                origin.say("You whisper to " + target.name + ": " + *message);
            }

            if (sent == 0) {
                return Result{ 0, false };
            }
            return Result{ sent, true };
        }

        void install_message_grammar(Node& root) {
            Node& msg = root.literal("msg");
            Node& targets = msg.argument("targets", Parser::players());
            targets.argument("message", Parser::greedy()).executes(run_message);
        }

    } // namespace

    void install_message(Registry& registry) {
        install_message_grammar(registry.root());
        // tell and w are the same command under another name in the game.
        Node& tell = registry.root().literal("tell");
        Node& tell_targets = tell.argument("targets", Parser::players());
        tell_targets.argument("message", Parser::greedy()).executes(run_message);

        Node& w = registry.root().literal("w");
        Node& w_targets = w.argument("targets", Parser::players());
        w_targets.argument("message", Parser::greedy()).executes(run_message);
    }

} // namespace terila::commands
