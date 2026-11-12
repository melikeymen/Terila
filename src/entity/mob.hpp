#pragma once

#include "log/logger.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace terila::entity {

    struct Mob {
        std::int32_t id = 0;
        std::array<std::uint8_t, 16> uuid{};
        std::int32_t type_id = 0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        float yaw = 0.0f;
        float head_yaw = 0.0f;
        int wander_ticks = 0;
        float wander_yaw = 0.0f;
    };

    // Terila keeps its own entity store; mobs wander over the flat world and are
    // streamed to players by the connection layer.
    class MobManager {
    public:
        void set_ground(double y) { ground_y_ = y; }

        bool spawn(const std::string& type_name,
            const std::unordered_map<std::string, std::int32_t>& types,
            double x, double z)
        {
            const auto it = types.find(type_name);
            if (it == types.end()) {
                LOG_WARN(Entity, "unknown entity type: {}", type_name);
                return false;
            }

            Mob mob;
            mob.id = next_id_++;
            mob.type_id = it->second;
            mob.x = x;
            mob.y = ground_y_;
            mob.z = z;
            mob.uuid[0] = 0x54;
            mob.uuid[1] = 0x65;
            mob.uuid[2] = 0x72;
            mob.uuid[3] = 0x69;
            for (int i = 4; i < 16; ++i) {
                mob.uuid[i] = static_cast<std::uint8_t>(next_random() & 0xFF);
            }
            // Shape the random tail like a version 4 UUID.
            mob.uuid[6] = static_cast<std::uint8_t>((mob.uuid[6] & 0x0F) | 0x40);
            mob.uuid[8] = static_cast<std::uint8_t>((mob.uuid[8] & 0x3F) | 0x80);

            mobs_.push_back(mob);
            LOG_DEBUG(Entity, "spawned {} as entity id={} at ({:.1f}, {:.1f})",
                type_name, mob.id, x, z);
            return true;
        }

        void tick() {
            for (auto& mob : mobs_) {
                if (--mob.wander_ticks <= 0) {
                    mob.wander_ticks = 40 + static_cast<int>(next_random() % 160);
                    mob.wander_yaw = static_cast<float>(next_random() % 360);
                }

                constexpr double kSpeed = 0.015;
                constexpr double kPi = 3.14159265358979323846;
                const double radians = static_cast<double>(mob.wander_yaw) * kPi / 180.0;
                const double nx = mob.x + std::sin(radians) * kSpeed;
                const double nz = mob.z - std::cos(radians) * kSpeed;

                // Only the area around spawn exists yet, so keep the herd nearby.
                if (std::abs(nx) > 40.0 || std::abs(nz) > 40.0) {
                    // fmod keeps the sign of its operand, so wrap into [0, 360) by hand.
                    float turned = mob.wander_yaw + 180.0f;
                    while (turned >= 360.0f) turned -= 360.0f;
                    while (turned < 0.0f) turned += 360.0f;
                    mob.wander_yaw = turned;
                    continue;
                }

                mob.x = nx;
                mob.z = nz;
                mob.y = ground_y_;
                mob.yaw = mob.wander_yaw;
                mob.head_yaw = mob.wander_yaw;
            }
        }

        const std::vector<Mob>& mobs() const { return mobs_; }

    private:
        std::uint32_t next_random() {
            rng_state_ = rng_state_ * 1664525u + 1013904223u;
            return rng_state_ >> 8;
        }

        std::vector<Mob> mobs_;
        double ground_y_ = 0.0;
        std::uint32_t rng_state_ = 0x9E3779B9u;
        std::int32_t next_id_ = 1;
    };

}
