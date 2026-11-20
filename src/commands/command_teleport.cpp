#include "commands/registry.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        // Moves every victim to the same point. Relative coordinates are offsets from the
        // victim's own position, which is what `tp @s ~ ~1 ~` means.
        Result teleport_to_position(Exec& exec, const std::vector<Target>& victims,
            const Position& position, const RotationValue* rotation) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            std::int32_t moved = 0;
            double last_x = 0.0;
            double last_y = 0.0;
            double last_z = 0.0;
            for (const auto& victim : victims) {
                double x = 0.0;
                double y = 0.0;
                double z = 0.0;
                resolve_position(position, victim.x, victim.y, victim.z, origin.yaw,
                    origin.pitch, x, y, z);
                last_x = x;
                last_y = y;
                last_z = z;

                float yaw = victim.yaw;
                float pitch = victim.pitch;
                if (rotation != nullptr) {
                    yaw = rotation->yaw_relative ? victim.yaw + rotation->yaw : rotation->yaw;
                    pitch = rotation->pitch_relative ? victim.pitch + rotation->pitch
                                                     : rotation->pitch;
                }
                if (origin.game->teleport(victim.entity, x, y, z, yaw, pitch)) {
                    ++moved;
                }
            }

            if (moved == 0) {
                return Result{ 0, false };
            }
            if (victims.size() == 1) {
                origin.say(std::format("Teleported {} to {:.6f}, {:.6f}, {:.6f}",
                    victims.front().name, last_x, last_y, last_z));
            }
            else {
                origin.say(std::format("Teleported {} entities to {:.6f}, {:.6f}, {:.6f}",
                    moved, last_x, last_y, last_z));
            }
            return Result{ moved, true };
        }

        // /tp <targets> <destination>: every victim lands on that entity, facing the way it
        // faces.
        Result teleport_to_target(Exec& exec, const std::vector<Target>& victims,
            const Target& destination) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            std::int32_t moved = 0;
            for (const auto& victim : victims) {
                if (origin.game->teleport(victim.entity, destination.x, destination.y,
                        destination.z, destination.yaw, destination.pitch)) {
                    ++moved;
                }
            }
            if (moved == 0) {
                return Result{ 0, false };
            }
            if (victims.size() == 1) {
                origin.say("Teleported " + victims.front().name + " to " + destination.name);
            }
            else {
                origin.say(std::format("Teleported {} entities to {}", moved,
                    destination.name));
            }
            return Result{ moved, true };
        }

        std::vector<Target> victims_of(Exec& exec, bool& ok) {
            ok = true;
            if (const auto* selector = exec.args.get<Selector>("targets")) {
                std::string error;
                auto targets = targets_of(*selector, exec.origin, error);
                if (targets.empty()) {
                    exec.origin.say(error.empty() ? "No entity was found" : error);
                    ok = false;
                }
                return targets;
            }
            if (exec.origin.console) {
                exec.origin.say("A player is required to teleport");
                ok = false;
                return {};
            }
            return { Target{ exec.origin.entity, exec.origin.name, exec.origin.x,
                exec.origin.y, exec.origin.z, exec.origin.yaw, exec.origin.pitch } };
        }

        Result run_self_position(Exec& exec) {
            const auto* position = exec.args.get<Position>("location");
            if (position == nullptr) {
                return Result{ 0, false };
            }
            bool ok = false;
            const auto victims = victims_of(exec, ok);
            if (!ok) {
                return Result{ 0, false };
            }
            return teleport_to_position(exec, victims, *position, nullptr);
        }

        Result run_targets_position(Exec& exec) {
            const auto* position = exec.args.get<Position>("location");
            if (position == nullptr) {
                return Result{ 0, false };
            }
            bool ok = false;
            const auto victims = victims_of(exec, ok);
            if (!ok) {
                return Result{ 0, false };
            }
            const auto* rotation = exec.args.get<RotationValue>("rotation");
            return teleport_to_position(exec, victims, *position, rotation);
        }

        Result run_self_target(Exec& exec) {
            const auto* selector = exec.args.get<Selector>("destination");
            if (selector == nullptr) {
                return Result{ 0, false };
            }
            std::string error;
            const auto matches = targets_of(*selector, exec.origin, error);
            if (matches.size() != 1) {
                exec.origin.say(matches.empty() ? error
                                                : "Expected one entity, found several");
                return Result{ 0, false };
            }
            if (exec.origin.console) {
                exec.origin.say("A player is required to teleport");
                return Result{ 0, false };
            }
            const std::vector<Target> victims{ Target{ exec.origin.entity, exec.origin.name,
                exec.origin.x, exec.origin.y, exec.origin.z, exec.origin.yaw,
                exec.origin.pitch } };
            return teleport_to_target(exec, victims, matches.front());
        }

        Result run_targets_target(Exec& exec) {
            const auto* selector = exec.args.get<Selector>("destination");
            if (selector == nullptr) {
                return Result{ 0, false };
            }
            std::string error;
            const auto matches = targets_of(*selector, exec.origin, error);
            if (matches.size() != 1) {
                exec.origin.say(matches.empty() ? error
                                                : "Expected one entity, found several");
                return Result{ 0, false };
            }
            bool ok = false;
            const auto victims = victims_of(exec, ok);
            if (!ok) {
                return Result{ 0, false };
            }
            return teleport_to_target(exec, victims, matches.front());
        }

        void add_teleport_grammar(Node& teleport) {
            teleport.argument("location", Parser::position()).executes(run_self_position);
            teleport.argument("destination", Parser::one_player()).executes(run_self_target);

            Node& targets = teleport.argument("targets", Parser::players());
            Node& location = targets.argument("location", Parser::position());
            location.executes(run_targets_position);
            location.argument("rotation", Parser::rotation()).executes(run_targets_position);
            targets.argument("destination", Parser::one_player()).executes(run_targets_target);
        }

    } // namespace

    void install_teleport(Registry& registry) {
        // tp is the game's alias for teleport; the tree repeats the grammar instead of
        // redirecting, so the client gets branches it can walk on its own.
        add_teleport_grammar(registry.root().literal("teleport"));
        add_teleport_grammar(registry.root().literal("tp"));
    }

} // namespace terila::commands
