#include "commands/registry.hpp"

#include <cstdint>
#include <string>

namespace terila::commands {

    namespace {

        Result run_summon(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            const auto* type = exec.args.get<std::string>("entity");
            if (type == nullptr) {
                return Result{ 0, false };
            }
            if (!origin.game->summonable(*type)) {
                origin.say("Unknown entity type '" + *type + "'");
                return Result{ 0, false };
            }

            double x = origin.x;
            double y = origin.y;
            double z = origin.z;
            if (const auto* position = exec.args.get<Position>("pos")) {
                resolve_position(*position, origin.x, origin.y, origin.z, origin.yaw,
                    origin.pitch, x, y, z);
            }

            if (!origin.game->summon(*type, x, y, z)) {
                origin.say("Unable to summon entity");
                return Result{ 0, false };
            }
            origin.say("Summoned new " + *type);
            return Result{ 1, true };
        }

    } // namespace

    void install_summon(Registry& registry) {
        Node& summon = registry.root().literal("summon");
        Node& entity = summon.argument("entity", Parser::resource_location());
        entity.executes(run_summon);
        entity.argument("pos", Parser::position()).executes(run_summon);
    }

} // namespace terila::commands
