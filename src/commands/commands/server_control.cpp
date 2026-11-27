#include "commands/registry.hpp"

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

    } // namespace

    void install_server_control(Registry& registry) {
        registry.root().literal("stop", Capability::Owner).executes(run_stop);
    }

} // namespace terila::commands
