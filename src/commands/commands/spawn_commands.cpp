#include "commands/registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        // A spawn point is always a block: the command names a corner, the game respawns
        // at the middle of that block.
        bool block_centre(const Position& position, const Origin& origin, double& x,
            double& y, double& z) {
            double fx = 0.0;
            double fy = 0.0;
            double fz = 0.0;
            resolve_position(position, origin.x, origin.y, origin.z, origin.yaw, origin.pitch,
                fx, fy, fz);
            if (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(fz)) {
                return false;
            }
            x = std::floor(fx) + 0.5;
            y = std::floor(fy);
            z = std::floor(fz) + 0.5;
            return true;
        }

        bool facing(const RotationValue* rotation, const Origin& origin, float& yaw,
            float& pitch) {
            if (rotation == nullptr) {
                yaw = origin.yaw;
                pitch = origin.pitch;
                return true;
            }
            yaw = rotation->yaw_relative ? origin.yaw + rotation->yaw : rotation->yaw;
            pitch = rotation->pitch_relative ? origin.pitch + rotation->pitch : rotation->pitch;
            yaw = std::fmod(yaw, 360.0f);
            if (yaw < -180.0f) {
                yaw += 360.0f;
            }
            if (yaw > 180.0f) {
                yaw -= 360.0f;
            }
            pitch = std::clamp(pitch, -90.0f, 90.0f);
            return true;
        }

        Result run_spawnpoint(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            double x = std::floor(origin.x) + 0.5;
            double y = std::floor(origin.y);
            double z = std::floor(origin.z) + 0.5;
            if (const auto* position = exec.args.get<Position>("pos")) {
                if (!block_centre(*position, origin, x, y, z)) {
                    origin.say("The position is outside the world");
                    return Result{ 0, false };
                }
            }

            float yaw = origin.yaw;
            float pitch = origin.pitch;
            facing(exec.args.get<RotationValue>("rotation"), origin, yaw, pitch);

            std::vector<Target> players;
            if (const auto* selector = exec.args.get<Selector>("targets")) {
                std::string error;
                players = targets_of(*selector, origin, error);
                if (players.empty()) {
                    origin.say(error.empty() ? "No player was found" : error);
                    return Result{ 0, false };
                }
            }
            else {
                if (origin.console) {
                    origin.say("A player is required to set a spawn point");
                    return Result{ 0, false };
                }
                players.push_back(Target{ origin.entity, origin.name, origin.x, origin.y,
                    origin.z });
            }

            std::int32_t set = 0;
            for (const auto& player : players) {
                if (!origin.game->set_spawn_point(player.entity, x, y, z, yaw, pitch)) {
                    continue;
                }
                ++set;
            }

            if (set == 0) {
                origin.say("The spawn point could not be set");
                return Result{ 0, false };
            }
            origin.say(std::format("Set spawn point to {:.1f}, {:.1f}, {:.1f} ({:.1f}, {:.1f})",
                x, y, z, yaw, pitch));
            return Result{ set, true };
        }

        Result run_setworldspawn(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            double x = std::floor(origin.x) + 0.5;
            double y = std::floor(origin.y);
            double z = std::floor(origin.z) + 0.5;
            if (const auto* position = exec.args.get<Position>("pos")) {
                if (!block_centre(*position, origin, x, y, z)) {
                    origin.say("The position is outside the world");
                    return Result{ 0, false };
                }
            }

            float yaw = origin.yaw;
            float pitch = origin.pitch;
            facing(exec.args.get<RotationValue>("rotation"), origin, yaw, pitch);

            if (!origin.game->set_world_spawn(x, y, z, yaw, pitch)) {
                origin.say("The world spawn point could not be set");
                return Result{ 0, false };
            }
            origin.say(std::format(
                "Set the world spawn point to {:.1f}, {:.1f}, {:.1f} ({:.1f}, {:.1f})", x, y, z,
                yaw, pitch));
            return Result{ 1, true };
        }

    } // namespace

    void install_spawn(Registry& registry) {
        Node& spawnpoint = registry.root().literal("spawnpoint", Capability::Operator);
        spawnpoint.executes(run_spawnpoint);
        Node& targets = spawnpoint.argument("targets", Parser::players());
        targets.executes(run_spawnpoint);
        Node& positioned = targets.argument("pos", Parser::position());
        positioned.executes(run_spawnpoint);
        positioned.argument("rotation", Parser::rotation()).executes(run_spawnpoint);

        Node& world = registry.root().literal("setworldspawn", Capability::Operator);
        world.executes(run_setworldspawn);
        Node& point = world.argument("pos", Parser::position());
        point.executes(run_setworldspawn);
        point.argument("rotation", Parser::rotation()).executes(run_setworldspawn);
    }

} // namespace terila::commands
