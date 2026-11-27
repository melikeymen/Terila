#include "commands/registry.hpp"

#include <cstdint>
#include <string>

namespace terila::commands {

    namespace {

        std::string speaker_of(const Origin& origin) {
            return origin.console ? "Server" : origin.name;
        }

        Result run_say(Exec& exec) {
            Origin& origin = exec.origin;
            const auto* message = exec.args.get<std::string>("message");
            if (message == nullptr || origin.game == nullptr) {
                return Result{ 0, false };
            }

            const std::size_t reached =
                origin.game->broadcast("[" + speaker_of(origin) + "] " + *message);
            return Result{ static_cast<std::int32_t>(reached), reached != 0 };
        }

        Result run_me(Exec& exec) {
            Origin& origin = exec.origin;
            const auto* action = exec.args.get<std::string>("action");
            if (action == nullptr || origin.game == nullptr) {
                return Result{ 0, false };
            }

            const std::size_t reached =
                origin.game->broadcast("* " + speaker_of(origin) + " " + *action);
            return Result{ static_cast<std::int32_t>(reached), reached != 0 };
        }

    } // namespace

    void install_say(Registry& registry) {
        Node& say = registry.root().literal("say");
        say.argument("message", Parser::message()).executes(run_say);

        Node& me = registry.root().literal("me");
        me.argument("action", Parser::message()).executes(run_me);
    }

} // namespace terila::commands
