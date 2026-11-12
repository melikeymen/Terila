#pragma once

#include "log/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace terila::entity {

    struct PlayerPos {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct Mob {
        std::int32_t id = 0;
        std::array<std::uint8_t, 16> uuid{};
        std::int32_t type_id = 0;
        std::string type_name;
        bool hostile = false;

        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        float yaw = 0.0f;
        float head_yaw = 0.0f;

        std::int32_t health = 10;
        std::int32_t max_health = 10;
        bool dead = false;
        int death_ticks = 0;

        // Bumped on every hit so each viewer can send its own hurt animation once.
        std::uint32_t hurt_serial = 0;

        // Steering state.
        int wander_ticks = 0;
        float wander_yaw = 0.0f;
        int flee_ticks = 0;
        float flee_yaw = 0.0f;
        double knockback = 0.0;
        float knockback_yaw = 0.0f;
    };

    // Degrees around +Y, matching the (sin, -cos) movement convention used below.
    inline float yaw_between(double from_x, double from_z, double to_x, double to_z) {
        constexpr double kPi = 3.14159265358979323846;
        const double dx = to_x - from_x;
        const double dz = to_z - from_z;
        double degrees = std::atan2(dx, -dz) * 180.0 / kPi;
        while (degrees < 0.0) degrees += 360.0;
        while (degrees >= 360.0) degrees -= 360.0;
        return static_cast<float>(degrees);
    }

    // Terila keeps its own entity store; mobs wander over the flat world, react to
    // damage and are streamed to players by the connection layer.
    class MobManager {
    public:
        void set_ground(double y) { ground_y_ = y; }

        bool spawn(const std::string& type_name,
            const std::unordered_map<std::string, std::int32_t>& types,
            double x, double z, bool hostile = false, std::int32_t max_health = 10)
        {
            const auto it = types.find(type_name);
            if (it == types.end()) {
                LOG_WARN(Entity, "unknown entity type: {}", type_name);
                return false;
            }

            Mob mob;
            mob.id = next_id_++;
            mob.type_id = it->second;
            mob.type_name = type_name;
            mob.hostile = hostile;
            mob.x = x;
            mob.y = ground_y_;
            mob.z = z;
            mob.max_health = max_health;
            mob.health = max_health;

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

            mobs_.push_back(std::move(mob));
            LOG_DEBUG(Entity, "spawned {} as entity id={} at ({:.1f}, {:.1f}) hostile={} hp={}",
                type_name, mobs_.back().id, x, z, hostile, max_health);
            return true;
        }

        const Mob* find(std::int32_t id) const {
            for (const auto& mob : mobs_) {
                if (mob.id == id) return &mob;
            }
            return nullptr;
        }

        // Applies damage and starts the knockback/flee reaction. away_yaw points
        // from the attacker towards the mob, so the mob is pushed along it.
        bool damage(std::int32_t id, std::int32_t amount, float away_yaw) {
            for (auto& mob : mobs_) {
                if (mob.id != id) continue;
                if (mob.dead) return false;

                mob.health -= amount;
                ++mob.hurt_serial;
                mob.knockback = kKnockbackDistance;
                mob.knockback_yaw = away_yaw;
                mob.flee_ticks = kFleeTicks;
                mob.flee_yaw = away_yaw;

                if (mob.health <= 0) {
                    mob.health = 0;
                    mob.dead = true;
                    mob.death_ticks = kDeathTicks;
                    LOG_DEBUG(Entity, "{} (id={}) died", mob.type_name, mob.id);
                }
                else {
                    LOG_DEBUG(Entity, "{} (id={}) hit for {}, hp={}/{}",
                        mob.type_name, mob.id, amount, mob.health, mob.max_health);
                }
                return true;
            }
            return false;
        }

        void tick(const std::vector<PlayerPos>& players) {
            for (auto& mob : mobs_) {
                if (mob.dead) {
                    if (mob.death_ticks > 0) --mob.death_ticks;
                    continue;
                }

                // Knockback wins over steering while it lasts.
                if (mob.knockback > 0.0) {
                    const double step = std::min(mob.knockback, kKnockbackStep);
                    mob.knockback -= step;
                    if (!move(mob, mob.knockback_yaw, step)) {
                        mob.knockback = 0.0;
                    }
                    mob.yaw = mob.knockback_yaw;
                    mob.head_yaw = mob.knockback_yaw;
                    continue;
                }

                if (mob.flee_ticks > 0) {
                    --mob.flee_ticks;
                    steer(mob, mob.flee_yaw, kFleeSpeed);
                    continue;
                }

                if (mob.hostile) {
                    if (const auto* target = nearest_player(players, mob, kChaseRange)) {
                        const float yaw = yaw_between(mob.x, mob.z, target->x, target->z);
                        steer(mob, yaw, kChaseSpeed);
                        continue;
                    }
                }

                if (--mob.wander_ticks <= 0) {
                    mob.wander_ticks = 40 + static_cast<int>(next_random() % 160);
                    mob.wander_yaw = static_cast<float>(next_random() % 360);
                }
                steer(mob, mob.wander_yaw, kSpeed);
            }

            // Drop the corpses once the death animation has played out.
            mobs_.erase(std::remove_if(mobs_.begin(), mobs_.end(),
                [](const Mob& m) { return m.dead && m.death_ticks <= 0; }),
                mobs_.end());
        }

        const std::vector<Mob>& mobs() const { return mobs_; }

        std::size_t living_count() const {
            return static_cast<std::size_t>(std::count_if(mobs_.begin(), mobs_.end(),
                [](const Mob& m) { return !m.dead; }));
        }

    private:
        static constexpr double kSpeed = 0.015;
        static constexpr double kFleeSpeed = 0.06;
        static constexpr double kChaseSpeed = 0.045;
        static constexpr double kKnockbackDistance = 0.6;
        static constexpr double kKnockbackStep = 0.15;
        static constexpr int    kFleeTicks = 60;
        static constexpr int    kDeathTicks = 20;
        static constexpr double kChaseRange = 16.0;

        void steer(Mob& mob, float yaw, double speed) {
            mob.yaw = yaw;
            mob.head_yaw = yaw;
            mob.wander_yaw = yaw;
            move(mob, yaw, speed);
        }

        // Only the area around spawn exists yet, so the herd is kept nearby.
        bool move(Mob& mob, float yaw, double distance) {
            constexpr double kPi = 3.14159265358979323846;
            const double radians = static_cast<double>(yaw) * kPi / 180.0;
            const double nx = mob.x + std::sin(radians) * distance;
            const double nz = mob.z - std::cos(radians) * distance;
            if (std::abs(nx) > 48.0 || std::abs(nz) > 48.0) {
                return false;
            }
            mob.x = nx;
            mob.z = nz;
            mob.y = ground_y_;
            return true;
        }

        static const PlayerPos* nearest_player(const std::vector<PlayerPos>& players,
            const Mob& mob, double range) {
            const PlayerPos* best = nullptr;
            double best_sq = range * range;
            for (const auto& player : players) {
                const double dx = player.x - mob.x;
                const double dz = player.z - mob.z;
                const double dist_sq = dx * dx + dz * dz;
                if (dist_sq <= best_sq) {
                    best_sq = dist_sq;
                    best = &player;
                }
            }
            return best;
        }

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
