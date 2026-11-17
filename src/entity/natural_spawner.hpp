#pragma once

// Natural spawning, the way ServerChunkCache/NaturalSpawner do it: a position is offered to
// the biome that is actually there, the biome's weighted list picks a mob, SpawnPlacements
// decides whether that mob may stand at the spot at all (ground, water, obstruction), its
// checkSpawnRules predicate decides the light, and the category only spawns while it is
// below its cap. The lists, weights, pack sizes and per-type placement are the game's own
// data (src/world/biome_spawns.hpp and the SpawnPlacements table in src/entity/mob_data.hpp);
// nothing here invents a spawn rule.

#include "entity/mob.hpp"
#include "world/biome_spawns.hpp"
#include "world/terrain_biomes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
        case data::MobCategory::UndergroundWaterCreature: return CategoryCaps::water_creature;
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
        if (name == "underground_water_creature") {
            return data::MobCategory::UndergroundWaterCreature;
        }
        if (name == "axolotls") return data::MobCategory::Axolotls;
        return std::nullopt;
    }

    inline bool is_water_category(data::MobCategory category) noexcept {
        return category == data::MobCategory::WaterCreature ||
            category == data::MobCategory::WaterAmbient ||
            category == data::MobCategory::UndergroundWaterCreature ||
            category == data::MobCategory::Axolotls;
    }

    // What the spawner needs to know about one block. Vanilla asks three questions: is the
    // position an empty spawn block (no collision, no fluid), is the block below a valid
    // spawn surface (sturdy, not a light source), and is that surface the animals tag.
    // The world classifies a block once (terrain::BlockKind) and the spawner reads it here.
    using SpawnBlock = world::terrain::BlockKind;
    using world::terrain::BlockKind;

    // How the spawner sees the world; the server wires these to its generator.
    struct SpawnWorld {
        std::function<int(std::int32_t x, std::int32_t z)> biome_index = {};
        // The y a mob stands at in a column (the first block above the terrain).
        std::function<int(std::int32_t x, std::int32_t z)> surface_y = {};
        std::function<bool(std::int32_t x, std::int32_t z)> is_water = {};
        std::function<int(std::int32_t x, std::int32_t y, std::int32_t z)> block_light = {};
        std::function<int(std::int32_t x, std::int32_t y, std::int32_t z)> sky_light = {};
        std::function<SpawnBlock(std::int32_t x, std::int32_t y, std::int32_t z)> block_kind = {};
        int sea_level = 63;
    };

    class NaturalSpawner {
    public:
        void set_world(SpawnWorld world) { world_ = std::move(world); }

        // Vanilla refuses to spawn within 24 blocks of the world spawn, not only near a player.
        void set_spawn_point(std::int32_t x, std::int32_t z) {
            spawn_x_ = x;
            spawn_z_ = z;
            has_spawn_point_ = true;
        }

        // The population a fresh world starts with: the area around the spawn is offered to
        // the biome of each position, in packs, until every category has reached its cap.
        std::size_t spawn_initial(MobManager& mobs, std::int32_t center_x, std::int32_t center_z,
            double radius, std::uint32_t& rng, bool night) {
            std::array<std::size_t, 8> placed{};
            std::size_t spawned = 0;

            for (int attempt = 0; attempt < kInitialAttempts; ++attempt) {
                const double angle = static_cast<double>(random(rng) % 360) *
                    3.14159265358979323846 / 180.0;
                const double distance = radius * static_cast<double>(random(rng) % 1000) / 1000.0;
                const double x = static_cast<double>(center_x) + 0.5 + std::cos(angle) * distance;
                const double z = static_cast<double>(center_z) + 0.5 + std::sin(angle) * distance;

                const int biome = world_.biome_index
                    ? world_.biome_index(static_cast<std::int32_t>(std::floor(x)),
                        static_cast<std::int32_t>(std::floor(z)))
                    : -1;
                const auto* list = list_for(biome);
                if (list == nullptr) {
                    continue;
                }

                const auto* entry = weighted_entry(list, rng, night);
                if (entry == nullptr) {
                    continue;
                }
                const auto category = category_from(entry->category);
                if (!category) {
                    continue;
                }
                if (placed[static_cast<std::size_t>(*category)] >=
                    static_cast<std::size_t>(cap_for(*category))) {
                    continue;
                }

                // AgeableMob#getBabySpawnChance: one animal in twenty is a baby.
                const bool baby = *category != data::MobCategory::Monster &&
                    (random(rng) % 100) < 5;
                if (spawn_one(mobs, std::string(entry->type), x, z, night, baby)) {
                    ++placed[static_cast<std::size_t>(*category)];
                    ++spawned;
                }
            }
            return spawned;
        }

        // Vanilla's monster rules: a dark enough spot, never within 24 blocks of a player or
        // of the world spawn, and only the categories the time of day allows.
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
        static constexpr int kInitialAttempts = 600;
        static constexpr double kMinPlayerDistance = 24.0;   // 576 squared, like vanilla
        static constexpr double kMinSpawnPointDistance = 24.0;
        static constexpr double kAreaRadius = 128.0;
        static constexpr int kNightSkyDarken = 11;           // level.getSkyDarken() at midnight
        static constexpr int kMaxStepUp = 4;                 // snow, leaves, a tree trunk
        static constexpr int kUndergroundWaterMaxY = 30;     // glow squid's depth

        static std::uint32_t random(std::uint32_t& rng) {
            rng = rng * 1664525u + 1013904223u;
            return rng >> 8;
        }

        static const data::BiomeSpawnList* list_for(int biome) {
            if (biome < 0 || biome >= static_cast<int>(world::terrain::kBiomeCount)) {
                return nullptr;
            }
            return data::find_biome_spawns(world::terrain::kBiomeNames[biome]);
        }

        // Vanilla rolls a category, then a type inside it. The time of day gates the category
        // so a daytime attempt never spends its roll on a monster; the light rule of the
        // chosen type still has the final say.
        const data::BiomeSpawn* weighted_entry(const data::BiomeSpawnList* list,
            std::uint32_t& rng, bool night) const {
            int total = 0;
            for (int i = 0; i < list->count; ++i) {
                const auto& entry = data::kBiomeSpawns[list->begin + i];
                const auto category = category_from(entry.category);
                if (!category || ((*category == data::MobCategory::Monster) != night)) {
                    continue;
                }
                total += entry.weight;
            }
            if (total == 0) {
                return nullptr;
            }

            int roll = static_cast<int>(random(rng) % static_cast<std::uint32_t>(total));
            for (int i = 0; i < list->count; ++i) {
                const auto& entry = data::kBiomeSpawns[list->begin + i];
                const auto category = category_from(entry.category);
                if (!category || ((*category == data::MobCategory::Monster) != night)) {
                    continue;
                }
                roll -= entry.weight;
                if (roll < 0) {
                    return &entry;
                }
            }
            return nullptr;
        }

        void try_spawn(MobManager& mobs, const std::vector<PlayerPos>& players, double x,
            double z, bool night, std::uint32_t& rng) {
            for (const auto& player : players) {
                if (distance_squared(x, z, player.x, player.z) <
                    kMinPlayerDistance * kMinPlayerDistance) {
                    return;
                }
            }
            if (has_spawn_point_ &&
                distance_squared(x, z, static_cast<double>(spawn_x_),
                    static_cast<double>(spawn_z_)) <
                    kMinSpawnPointDistance * kMinSpawnPointDistance) {
                return;
            }

            const int biome = world_.biome_index
                ? world_.biome_index(static_cast<std::int32_t>(std::floor(x)),
                    static_cast<std::int32_t>(std::floor(z)))
                : -1;
            const auto* list = list_for(biome);
            if (list == nullptr) {
                return;
            }

            const auto* chosen = weighted_entry(list, rng, night);
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
                const bool baby = *category != data::MobCategory::Monster &&
                    (random(rng) % 100) < 5;
                spawn_one(mobs, std::string(chosen->type), x + offset_x, z + offset_z, night,
                    baby);
            }
        }

        // One spawn attempt: the placement and light rules of the type decide, and only then
        // does the mob exist.
        bool spawn_one(MobManager& mobs, const std::string& type, double x, double z,
            bool night, bool baby) const {
            const auto* spec = data::find_mob_by_name(type);
            if (spec == nullptr) {
                return false;
            }

            const auto bx = static_cast<std::int32_t>(std::floor(x));
            const auto bz = static_cast<std::int32_t>(std::floor(z));
            const int y = spawn_y(bx, bz);

            if (!place_allowed(*spec, bx, y, bz)) {
                return false;
            }
            if (!light_allowed(*spec, bx, y, bz, night)) {
                return false;
            }
            return mobs.spawn(type, x, z, baby);
        }

        // The block a mob would stand in: the column's surface, stepped up over the thin
        // layer (snow, leaves) vanilla's heightmap also steps over.
        int spawn_y(std::int32_t x, std::int32_t z) const {
            int y = world_.surface_y ? world_.surface_y(x, z) : 64;
            for (int step = 0; step < kMaxStepUp; ++step) {
                const SpawnBlock kind = kind_at(x, y, z);
                if (kind == SpawnBlock::Air || kind == SpawnBlock::Water) {
                    break;
                }
                ++y;
            }
            return y;
        }

        SpawnBlock kind_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            if (world_.block_kind) {
                return world_.block_kind(x, y, z);
            }
            // Without a block resolver the world is a plain: the surface block is the soil.
            const int surface = world_.surface_y ? world_.surface_y(x, z) : 64;
            if (world_.is_water && world_.is_water(x, z)) {
                return y <= surface ? SpawnBlock::Water : SpawnBlock::Air;
            }
            if (y < surface) {
                return y == surface - 1 ? SpawnBlock::Soil : SpawnBlock::Solid;
            }
            return SpawnBlock::Air;
        }

        // SpawnPlacementTypes: ON_GROUND wants an empty block on a sturdy surface, IN_WATER
        // wants the fluid with no full block above, and the animal rule also wants the
        // animals_spawnable_on surface. This is what keeps cows out of the ocean and fish out
        // of a desert.
        bool place_allowed(const data::MobSpec& spec, std::int32_t x, std::int32_t y,
            std::int32_t z) const {
            const SpawnBlock here = kind_at(x, y, z);
            const SpawnBlock below = kind_at(x, y - 1, z);
            const SpawnBlock above = kind_at(x, y + 1, z);

            switch (spec.placement) {
            case data::SpawnPlacement::InWater: {
                if (here != SpawnBlock::Water) {
                    return false;
                }
                if (above == SpawnBlock::Solid || above == SpawnBlock::Soil) {
                    return false;
                }
                // GlowSquid#checkGlowSquidSpawnRules: an underground water creature only
                // appears below y=30, which this world has only in its deep ocean floors.
                if (spec.category == data::MobCategory::UndergroundWaterCreature &&
                    y > kUndergroundWaterMaxY) {
                    return false;
                }
                // WaterAnimal#checkSurfaceWaterAnimalSpawnRules: the water ambient pack (cod,
                // salmon, pufferfish, tropical fish) stays near the surface, with water on
                // both sides of it.
                if (spec.category == data::MobCategory::WaterAmbient) {
                    if (above != SpawnBlock::Water || below != SpawnBlock::Water) {
                        return false;
                    }
                    if (y < world_.sea_level - 13 || y > world_.sea_level) {
                        return false;
                    }
                }
                return true;
            }
            case data::SpawnPlacement::InLava:
                // This world has no lava lakes yet, so nothing can satisfy the rule.
                return false;
            case data::SpawnPlacement::OnGround: {
                if (here != SpawnBlock::Air) {
                    return false;
                }
                if (above != SpawnBlock::Air && above != SpawnBlock::Plant) {
                    return false;
                }
                if (below != SpawnBlock::Solid && below != SpawnBlock::Soil) {
                    return false;
                }
                // Animal#checkAnimalSpawnRules: the surface has to be in the
                // animals_spawnable_on tag, which is grass_block.
                return spec.light != data::SpawnLight::Bright || below == SpawnBlock::Soil;
            }
            case data::SpawnPlacement::NoRestrictions:
            default:
                return here == SpawnBlock::Air;
            }
        }

        // Monster#isDarkEnoughToSpawn and Animal#isBrightEnoughToSpawn, on the light of the
        // spawn position. The sky darkens at night, which is what lets monsters appear on the
        // surface after dusk.
        bool light_allowed(const data::MobSpec& spec, std::int32_t x, std::int32_t y,
            std::int32_t z, bool night) const {
            if (spec.light == data::SpawnLight::Any) {
                return true;
            }
            const int sky = world_.sky_light ? world_.sky_light(x, y, z) : 15;
            const int block = world_.block_light ? world_.block_light(x, y, z) : 0;
            const int sky_now = night ? std::max(0, sky - kNightSkyDarken) : sky;
            const int brightness = std::max(sky_now, block);

            if (spec.light == data::SpawnLight::Dark) {
                // The overworld's monster spawn block light limit is 0, and the random light
                // test samples 0..7.
                return block == 0 && brightness < 8;
            }
            return brightness > 8;
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

        SpawnWorld world_;
        std::int32_t spawn_x_ = 0;
        std::int32_t spawn_z_ = 0;
        bool has_spawn_point_ = false;
    };

} // namespace terila::entity
