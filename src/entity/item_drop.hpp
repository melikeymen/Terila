#pragma once

#include "entity/mob.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace terila::entity {

    // A broken block that now lies on the ground as a minecraft:item entity.
    struct ItemDrop {
        std::int32_t id = 0;
        std::array<std::uint8_t, 16> uuid{};
        std::int32_t item_id = 0;
        std::int32_t count = 1;

        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double vx = 0.0;
        double vy = 0.0;
        double vz = 0.0;
        double ground_y = 0.0;

        int age_ticks = 0;

        // 0 while the drop waits on the ground, the collector's entity id once it was
        // picked up, and -1 when it simply timed out.
        std::int32_t collected_by = 0;
    };

    // Drops fall with a little horizontal drift, rest on the generated surface, are
    // collected by a nearby player and disappear after a few minutes otherwise.
    class ItemDropManager {
    public:
        std::int32_t spawn(std::int32_t item_id, std::int32_t count, double x, double y,
            double z, double ground_y) {
            ItemDrop drop;
            drop.id = next_id_++;
            drop.item_id = item_id;
            drop.count = count;
            drop.x = x;
            drop.y = y;
            drop.z = z;
            drop.ground_y = ground_y;
            drop.vy = kSpawnLift;
            drop.vx = (static_cast<double>(next_random() % 200) - 100.0) / 1600.0;
            drop.vz = (static_cast<double>(next_random() % 200) - 100.0) / 1600.0;

            for (auto& byte : drop.uuid) {
                byte = static_cast<std::uint8_t>(next_random() & 0xFF);
            }
            drop.uuid[6] = static_cast<std::uint8_t>((drop.uuid[6] & 0x0F) | 0x40);
            drop.uuid[8] = static_cast<std::uint8_t>((drop.uuid[8] & 0x3F) | 0x80);

            drops_.push_back(drop);
            LOG_DEBUG(Entity, "dropped item {} (entity id={}) at ({:.2f}, {:.2f}, {:.2f})",
                item_id, drop.id, drop.x, drop.y, drop.z);
            return drop.id;
        }

        void tick(const std::vector<PlayerPos>& players) {
            // Last tick's pickups have already been reported to the clients.
            drops_.erase(std::remove_if(drops_.begin(), drops_.end(),
                [](const ItemDrop& drop) { return drop.collected_by != 0; }),
                drops_.end());

            for (auto& drop : drops_) {
                physics(drop);

                if (++drop.age_ticks > kLifetimeTicks) {
                    drop.collected_by = -1;
                    continue;
                }
                if (drop.age_ticks < kPickupDelayTicks) {
                    continue;
                }

                for (const auto& player : players) {
                    const double dx = player.x - drop.x;
                    const double dy = player.y - drop.y;
                    const double dz = player.z - drop.z;
                    if (dx * dx + dz * dz > kPickupRadius * kPickupRadius) {
                        continue;
                    }
                    if (std::abs(dy) > kPickupHeight) {
                        continue;
                    }
                    drop.collected_by = (player.id == 0) ? -1 : player.id;
                    break;
                }
            }
        }

        const std::vector<ItemDrop>& drops() const { return drops_; }

        std::size_t count() const { return drops_.size(); }

    private:
        static constexpr double kSpawnLift = 0.12;
        static constexpr double kGravity = 0.04;
        static constexpr double kPickupRadius = 1.6;
        static constexpr double kPickupHeight = 2.0;
        static constexpr int kPickupDelayTicks = 8;
        static constexpr int kLifetimeTicks = 6000;

        void physics(ItemDrop& drop) {
            const bool resting = drop.y <= drop.ground_y + 1e-6 && drop.vy <= 0.0;
            if (resting) {
                drop.y = drop.ground_y;
                drop.vx = 0.0;
                drop.vz = 0.0;
                return;
            }

            drop.vy -= kGravity;
            drop.x += drop.vx;
            drop.y += drop.vy;
            drop.z += drop.vz;

            if (drop.y <= drop.ground_y) {
                drop.y = drop.ground_y;
                drop.vx = 0.0;
                drop.vy = 0.0;
                drop.vz = 0.0;
            }
        }

        std::uint32_t next_random() {
            rng_state_ = rng_state_ * 1664525u + 1013904223u;
            return rng_state_ >> 8;
        }

        std::vector<ItemDrop> drops_;
        std::uint32_t rng_state_ = 0x51ED270Bu;
        std::int32_t next_id_ = 5000;
    };

} // namespace terila::entity
