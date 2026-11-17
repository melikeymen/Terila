#pragma once

// Natural spawning, the way ServerChunkCache/NaturalSpawner do it: positions near the players
// are offered to the biome's spawn list, the list's weights pick a mob, the pack size decides
// how many arrive, and a category only spawns while it is below its cap. The lists, weights
// and pack sizes are the game's own data (src/world/biome_spawns.hpp); nothing here invents a
// spawn table.

#include "entity/mob.hpp"
#include "world/biome_spawns.hpp"
#include "world/terrain_biomes.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace terila::entity {

    // Vanilla's per-category caps, counted over the loaded area around a player.
    struct CategoryCaps {
        static constexpr int monster = 70;
        static constexpr int creature = 10;
        static constexpr int ambient = 15;
        static constexpr int water_creature = 5;
        static constexpr int water_ambient = 20;
        static constexpr int axolotls = 5;
    };

    inline int cap_for(data::MobCategory category) noexcept {
        switch (category) {
        case data::MobCategory::Monster: return CategoryCaps::monster;
        case data::MobCategory::Creature: return CategoryCaps::creature;
        case data::MobCategory::Ambient: return CategoryCaps::ambient;
        case data::MobCategory::WaterCreature: return CategoryCaps::water_creature;
        case data::MobCategory::WaterAmbient: return CategoryCaps::water_ambient;
        case data::MobCategory::Axolotls: return CategoryCaps::axolotls;
        default: return 0;
        }
    }

    inline std::optional<data::MobCategory> category_from(std::string_view name) noexcept {
        if (name == "monster") return data::MobCategory::Monster;
        if (name == "creature") return data::MobCategory::Creature;
        if (name == "ambient") return data::MobCategory::Ambient;
        if (name == "water_creature") return data::MobCategory::WaterCreature;
        if (name == "water_ambient") return data::MobCategory::WaterAmbient;
        if (name == "axolotls") return data::MobCategory::Axolotls;
        return std::nullopt;
    }

    inline bool is_water_category(data::MobCategory category) noexcept {
        return category == data::MobCategory::WaterCreature ||
            category == data::MobCategory::WaterAmbient ||
            category == data::MobCategory::UndergroundWaterCreature ||
            category == data::MobCategory::Axolotls;
    }

    // How the spawner sees the world; the server wires these to its generator.
    struct SpawnWorld {
        std::function<int(std::int32_t x, std::int32_t z)> biome_index = {};
        std::function<int(std::int32_t x, std::int32_t z)> surface_y = {};
        std::function<bool(std::int32_t x, std::int32_t z)> is_water = {};
        std::function<int(std::int32_t x, std::int32_t y, std::int32_t z)> block_light = {};
        std::function<int(std::int32_t x, std::int32_t y, std::int32_t z)> sky_light = {};
    };

    class NaturalSpawner {
    public:
        void set_world(SpawnWorld world) { world_ = std::move(world); }

        // The population a fresh world starts with: every category the biome lists is filled
        // up to its cap, in packs, the way ServerChunkCache seeds a new chunk area.
        std::size_t spawn_initial(MobManager& mobs, std::int32_t center_x, std::int32_t center_z,
            double radius, std::uint32_t& rng, bool night) {
            const int biome = world_.biome_index ? world_.biome_index(center_x, center_z) : -1;
            const auto* list = list_for(biome);
            if (list == nullptr) {
                return 0;
            }

            std::size_t spawned = 0;
            std::array<std::size_t, 8> placed{};
            for (int pass = 0; pass < kInitialPasses; ++pass) {
                for (int i = 0; i < list->count; ++i) {
                    const auto& entry = data::kBiomeSpawns[list->begin + i];
                    const auto category = category_from(entry.category);
                    if (!category) {
                        continue;
                    }
                    if ((*category == data::MobCategory::Monster) != night) {
                        // Daylight has no monsters in it; they arrive at dusk.
                        continue;
                    }
                    for (int pack = 0; pack < entry.min_count +
                        static_cast<int>(random(rng) % (entry.max_count - entry.min_count + 1));
                        ++pack) {
                        if (placed[static_cast<std::size_t>(*category)] >=
                            static_cast<std::size_t>(cap_for(*category))) {
                            break;
                        }
                        const double angle = static_cast<double>(random(rng) % 360) *
                            3.14159265358979323846 / 180.0;
                        const double distance = radius * static_cast<double>(random(rng) % 1000) / 1000.0;
                        const double x = static_cast<double>(center_x) + 0.5 + std::cos(angle) * distance;
                        const double z = static_cast<double>(center_z) + 0.5 + std::sin(angle) * distance;
                        if (!placeable(x, z, *category)) {
                            continue;
                        }
                        // AgeableMob#getBabySpawnChance: one animal in twenty is a baby.
                        const bool baby = *category != data::MobCategory::Monster &&
                            (random(rng) % 100) < 5;
                        if (mobs.spawn(std::string(entry.type), x, z, baby)) {
                            ++placed[static_cast<std::size_t>(*category)];
                            ++spawned;
                        }
                    }
                }
            }
            return spawned;
        }

        // Vanilla's monster rules: a dark enough spot (block light 0, sky light under 8),
        // never within 24 blocks of a player, and only creatures in daylight.
        void tick(MobManager& mobs, const std::vector<PlayerPos>& players, bool night,
            std::uint32_t& rng) {
            if (players.empty()) {
                return;
            }
            for (int attempt = 0; attempt < kAttemptsPerTick; ++attempt) {
                const auto& player = players[random(rng) % players.size()];
                const double x = player.x + static_cast<double>(random(rng) % 96) - 48.0;
                const double z = player.z + static_cast<double>(random(rng) % 96) - 48.0;
                try_spawn(mobs, players, x, z, night, rng);
            }
        }

    private:
        static constexpr int kAttemptsPerTick = 6;
        static constexpr int kInitialPasses = 2;
        static constexpr double kMinPlayerDistance = 24.0;
        static constexpr double kAreaRadius = 128.0;

        static std::uint32_t random(std::uint32_t& rng) {
            rng = rng * 1664525u + 1013904223u;
            return rng >> 8;
        }

        bool placeable(double x, double z, data::MobCategory category) const {
            const auto bx = static_cast<std::int32_t>(std::floor(x));
            const auto bz = static_cast<std::int32_t>(std::floor(z));
            const bool water = world_.is_water ? world_.is_water(bx, bz) : false;
            return is_water_category(category) ? water : !water;
        }

        void try_spawn(MobManager& mobs, const std::vector<PlayerPos>& players, double x,
            double z, bool night, std::uint32_t& rng) {
            for (const auto& player : players) {
                if (distance_squared(x, z, player.x, player.z) <
                    kMinPlayerDistance * kMinPlayerDistance) {
                    return;
                }
            }

            const int biome = world_.biome_index
                ? world_.biome_index(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z))
                : -1;
            const auto* list = list_for(biome);
            if (list == nullptr) {
                return;
            }

            const auto bx = static_cast<std::int32_t>(x);
            const auto bz = static_cast<std::int32_t>(z);
            const int surface = world_.surface_y ? world_.surface_y(bx, bz) : 64;
            const int block = world_.block_light ? world_.block_light(bx, surface, bz) : 0;
            const int sky = world_.sky_light ? world_.sky_light(bx, surface, bz) : 15;
            // Monsters want a dark spot; in daylight only the unlit ones qualify.
            const bool dark = block == 0 && sky < 8;
            const bool monster_weather = night || dark;

            const data::BiomeSpawn* chosen = nullptr;
            int total = 0;
            for (int i = 0; i < list->count; ++i) {
                const auto& entry = data::kBiomeSpawns[list->begin + i];
                const auto category = category_from(entry.category);
                if (!category || ((*category == data::MobCategory::Monster) != monster_weather)) {
                    continue;
                }
                total += entry.weight;
            }
            if (total == 0) {
                return;
            }

            int roll = static_cast<int>(random(rng) % static_cast<std::uint32_t>(total));
            for (int i = 0; i < list->count; ++i) {
                const auto& entry = data::kBiomeSpawns[list->begin + i];
                const auto category = category_from(entry.category);
                if (!category || ((*category == data::MobCategory::Monster) != monster_weather)) {
                    continue;
                }
                roll -= entry.weight;
                if (roll < 0) {
                    chosen = &entry;
                    break;
                }
            }
            if (chosen == nullptr) {
                return;
            }

            const auto category = category_from(chosen->category);
            if (!category || living_in_area(mobs, players, *category) >= cap_for(*category)) {
                return;
            }

            const int count = chosen->min_count +
                static_cast<int>(random(rng) % static_cast<std::uint32_t>(
                    chosen->max_count - chosen->min_count + 1));
            for (int i = 0; i < count; ++i) {
                // The pack is cut short when the category fills up: the cap is a limit, not a
                // suggestion, so a pack of four cannot take a category two over it.
                if (living_in_area(mobs, players, *category) >= cap_for(*category)) {
                    break;
                }
                const double offset_x = static_cast<double>(random(rng) % 13) - 6.0;
                const double offset_z = static_cast<double>(random(rng) % 13) - 6.0;
                if (!placeable(x + offset_x, z + offset_z, *category)) {
                    continue;
                }
                const bool baby = *category != data::MobCategory::Monster &&
                    (random(rng) % 100) < 5;
                mobs.spawn(std::string(chosen->type), x + offset_x, z + offset_z, baby);
            }
        }

        static std::size_t living_in_area(const MobManager& mobs,
            const std::vector<PlayerPos>& players, data::MobCategory category) {
            std::size_t count = 0;
            for (const auto& mob : mobs.mobs()) {
                if (mob.dead || mob.category() != category) {
                    continue;
                }
                for (const auto& player : players) {
                    if (distance_squared(mob.x, mob.z, player.x, player.z) <=
                        kAreaRadius * kAreaRadius) {
                        ++count;
                        break;
                    }
                }
            }
            return count;
        }

        static const data::BiomeSpawnList* list_for(int biome) {
            if (biome < 0 || biome >= static_cast<int>(world::terrain::kBiomeCount)) {
                return nullptr;
            }
            return data::find_biome_spawns(world::terrain::kBiomeNames[biome]);
        }

        SpawnWorld world_;
    };

} // namespace terila::entity
