#pragma once

// The natural population of the world: who lives where, and how much of it the world may
// hold. Every tick the chunks around the players are offered to the categories that still
// have room, the biome's own weighted list picks a species, that species' placement and
// light rules decide whether it may stand at the spot, and only then does the mob exist.
// The caps scale with the area that is actually spawnable, and nothing spawns in peaceful.

#include "entity/difficulty.hpp"
#include "entity/mob.hpp"
#include "world/biome_spawns.hpp"
#include "world/terrain_biomes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace terila::entity {

    using SpawnBlock = world::terrain::BlockKind;
    using world::terrain::BlockKind;

    // What the cycle needs from the world; the server wires these to its generator. Nothing
    // here reaches into chunks.
    struct SpawnWorld {
        std::function<int(std::int32_t x, std::int32_t z)> biome_index = {};
        std::function<int(std::int32_t x, std::int32_t z)> surface_y = {};
        std::function<bool(std::int32_t x, std::int32_t z)> is_water = {};
        std::function<int(std::int32_t x, std::int32_t y, std::int32_t z)> block_light = {};
        std::function<int(std::int32_t x, std::int32_t y, std::int32_t z)> sky_light = {};
        std::function<SpawnBlock(std::int32_t x, std::int32_t y, std::int32_t z)> block_kind = {};
        std::int32_t min_y = -64;
        int sea_level = 63;
    };

    class Population {
    public:
        void set_world(SpawnWorld world) { world_ = std::move(world); }
        void set_difficulty(Difficulty difficulty) { difficulty_ = difficulty; }
        void set_moon_brightness(float brightness) { moon_brightness_ = brightness; }
        void set_spawn_point(std::int32_t x, std::int32_t z) {
            spawn_x_ = x;
            spawn_z_ = z;
            has_spawn_point_ = true;
        }

        // The population a fresh world starts with. The cycle below does the work; this walks
        // a wider disc once so the first player does not meet an empty world.
        std::size_t initial(MobManager& mobs, std::int32_t center_x, std::int32_t center_z,
            double radius, std::uint32_t& rng, bool night) {
            std::size_t spawned = 0;
            for (int attempt = 0; attempt < kInitialAttempts; ++attempt) {
                const double angle = static_cast<double>(random(rng) % 360) *
                    3.14159265358979323846 / 180.0;
                const double distance = radius * static_cast<double>(random(rng) % 1000) / 1000.0;
                const double x = static_cast<double>(center_x) + 0.5 + std::cos(angle) * distance;
                const double z = static_cast<double>(center_z) + 0.5 + std::sin(angle) * distance;
                if (place_any(mobs, x, z, night, rng)) {
                    ++spawned;
                }
            }
            return spawned;
        }

        // One tick of the cycle. The chunk list is rebuilt from the players, capacities are
        // derived from its size, and only a rotating window of chunks is visited so the cost
        // per tick stays flat no matter how far the players have explored.
        void tick(MobManager& mobs, const std::vector<PlayerPos>& players, bool night,
            std::uint32_t& rng, std::int64_t game_time) {
            if (players.empty()) {
                return;
            }

            std::vector<std::pair<std::int32_t, std::int32_t>> chunks;
            std::unordered_set<std::uint64_t> seen;
            for (const auto& player : players) {
                const auto center_x = static_cast<std::int32_t>(std::floor(player.x)) >> 4;
                const auto center_z = static_cast<std::int32_t>(std::floor(player.z)) >> 4;
                for (int dz = -kSpawnChunkRadius; dz <= kSpawnChunkRadius; ++dz) {
                    for (int dx = -kSpawnChunkRadius; dx <= kSpawnChunkRadius; ++dx) {
                        const std::int32_t cx = center_x + dx;
                        const std::int32_t cz = center_z + dz;
                        if (seen.insert(chunk_key(cx, cz)).second) {
                            chunks.emplace_back(cx, cz);
                        }
                    }
                }
            }
            if (chunks.empty()) {
                return;
            }

            std::array<std::size_t, 8> counts{};
            count_categories(mobs, players, counts);

            // The room a category has: its per-chunk maximum scaled to the spawnable area.
            // 289 is 17 squared, the area the maximum is written for.
            const auto capacity = [size = chunks.size()](data::MobCategory category) {
                const int per_chunk = max_instances(category);
                if (per_chunk <= 0) {
                    return std::size_t{ 0 };
                }
                return static_cast<std::size_t>(per_chunk) * size / 289u;
            };

            const std::size_t window = std::min<std::size_t>(kChunksPerTick, chunks.size());
            for (std::size_t i = 0; i < window; ++i) {
                const auto [cx, cz] = chunks[(chunk_cursor_ + i) % chunks.size()];
                for (const auto category : kCategories) {
                    if (difficulty_ == Difficulty::Peaceful &&
                        category == data::MobCategory::Monster) {
                        continue;
                    }
                    const auto slot = static_cast<std::size_t>(category);
                    if (counts[slot] >= capacity(category)) {
                        continue;
                    }
                    attempt_chunk(mobs, players, cx, cz, category, night, rng, game_time,
                        counts[slot], capacity(category));
                }
            }
            chunk_cursor_ = (chunk_cursor_ + window) % chunks.size();
            age_local_ticks(players);
            bound_local_time(seen);
        }

        // How many chunks the local time is being tracked for. Exposed so the bound can be
        // checked without reaching into the map.
        std::size_t tracked_chunks() const noexcept { return local_time_.size(); }

        std::int64_t local_ticks(std::int32_t cx, std::int32_t cz) const {
            const auto found = local_time_.find(chunk_key(cx, cz));
            return found == local_time_.end() ? 0 : found->second;
        }

        // The per-category maximum from the game's own category table.
        static int max_instances(data::MobCategory category) noexcept {
            switch (category) {
            case data::MobCategory::Monster: return 70;
            case data::MobCategory::Creature: return 10;
            case data::MobCategory::Ambient: return 15;
            case data::MobCategory::WaterCreature: return 5;
            case data::MobCategory::UndergroundWaterCreature: return 5;
            case data::MobCategory::WaterAmbient: return 20;
            case data::MobCategory::Axolotls: return 5;
            default: return -1;
            }
        }

    private:
        static constexpr int kSpawnChunkRadius = 8;
        // The local time only matters near players, so the map is trimmed back to the chunks
        // that are being visited once it grows past this.
        static constexpr std::size_t kMaxTrackedChunks = 4096;
        static constexpr std::size_t kChunksPerTick = 32;
        static constexpr int kInitialAttempts = 600;
        static constexpr double kMinPlayerDistance = 24.0;
        static constexpr double kMinSpawnPointDistance = 24.0;
        static constexpr int kNightSkyDarken = 11;
        static constexpr int kUndergroundWaterMaxY = 30;
        static constexpr int kWaterFloorSearch = 12;

        static constexpr std::array<data::MobCategory, 7> kCategories = {
            data::MobCategory::Monster,
            data::MobCategory::Creature,
            data::MobCategory::Ambient,
            data::MobCategory::WaterCreature,
            data::MobCategory::WaterAmbient,
            data::MobCategory::UndergroundWaterCreature,
            data::MobCategory::Axolotls,
        };

        static std::uint32_t random(std::uint32_t& rng) {
            rng = rng * 1664525u + 1013904223u;
            return rng >> 8;
        }

        static std::uint64_t chunk_key(std::int32_t cx, std::int32_t cz) {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
                static_cast<std::uint32_t>(cz);
        }

        static std::optional<data::MobCategory> category_from(std::string_view name) {
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

        static const data::BiomeSpawnList* list_for(int biome) {
            if (biome < 0 || biome >= static_cast<int>(world::terrain::kBiomeCount)) {
                return nullptr;
            }
            return data::find_biome_spawns(world::terrain::kBiomeNames[biome]);
        }

        static std::size_t count_category(const MobManager& mobs, data::MobCategory category) {
            return static_cast<std::size_t>(std::count_if(mobs.mobs().begin(), mobs.mobs().end(),
                [category](const Mob& mob) {
                    return !mob.dead && mob.category() == category;
                }));
        }

        void count_categories(const MobManager& mobs, const std::vector<PlayerPos>& players,
            std::array<std::size_t, 8>& counts) const {
            // Counted over the chunk area the cycle spawns in, not a circular radius: a mob
            // that walked to the corner of that area still occupies its slot.
            for (const auto& mob : mobs.mobs()) {
                if (mob.dead || mob.persistent) {
                    continue;
                }
                const auto mob_cx = static_cast<std::int32_t>(std::floor(mob.x)) >> 4;
                const auto mob_cz = static_cast<std::int32_t>(std::floor(mob.z)) >> 4;
                for (const auto& player : players) {
                    const auto player_cx =
                        static_cast<std::int32_t>(std::floor(player.x)) >> 4;
                    const auto player_cz =
                        static_cast<std::int32_t>(std::floor(player.z)) >> 4;
                    if (std::abs(mob_cx - player_cx) <= kSpawnChunkRadius &&
                        std::abs(mob_cz - player_cz) <= kSpawnChunkRadius) {
                        ++counts[static_cast<std::size_t>(mob.category())];
                        break;
                    }
                }
            }
        }

        bool near_player_or_spawn(const std::vector<PlayerPos>& players, double x,
            double z) const {
            for (const auto& player : players) {
                if (distance_squared(x, z, player.x, player.z) <
                    kMinPlayerDistance * kMinPlayerDistance) {
                    return true;
                }
            }
            if (has_spawn_point_ && distance_squared(x, z,
                static_cast<double>(spawn_x_), static_cast<double>(spawn_z_)) <
                kMinSpawnPointDistance * kMinSpawnPointDistance) {
                return true;
            }
            return false;
        }

        static double distance_squared(double ax, double az, double bx, double bz) {
            const double dx = ax - bx;
            const double dz = az - bz;
            return dx * dx + dz * dz;
        }

        // A world can be explored for days; the counter per chunk must not follow it forever.
        // Once the map is over the bound, chunks nobody is near any more are dropped.
        void bound_local_time(const std::unordered_set<std::uint64_t>& live) {
            if (local_time_.size() <= kMaxTrackedChunks) {
                return;
            }
            for (auto it = local_time_.begin(); it != local_time_.end();) {
                if (live.count(it->first) == 0) {
                    it = local_time_.erase(it);
                }
                else {
                    ++it;
                }
            }
            while (local_time_.size() > kMaxTrackedChunks) {
                local_time_.erase(local_time_.begin());
            }
        }

        void age_local_ticks(const std::vector<PlayerPos>& players) {
            for (const auto& player : players) {
                const auto cx = static_cast<std::int32_t>(std::floor(player.x)) >> 4;
                const auto cz = static_cast<std::int32_t>(std::floor(player.z)) >> 4;
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        ++local_time_[chunk_key(cx + dx, cz + dz)];
                    }
                }
            }
        }

        void attempt_chunk(MobManager& mobs, const std::vector<PlayerPos>& players,
            std::int32_t cx, std::int32_t cz, data::MobCategory category, bool night,
            std::uint32_t& rng, std::int64_t game_time, std::size_t& count,
            std::size_t limit) {
            if (count >= limit) {
                return;
            }
            const auto start_x = cx * 16 + static_cast<std::int32_t>(random(rng) % 16);
            const auto start_z = cz * 16 + static_cast<std::int32_t>(random(rng) % 16);
            if (near_player_or_spawn(players, start_x + 0.5, start_z + 0.5)) {
                return;
            }

            const int biome = world_.biome_index ? world_.biome_index(start_x, start_z) : -1;
            const auto* list = list_for(biome);
            if (list == nullptr) {
                return;
            }
            const auto* entry = pick(list, category, rng);
            if (entry == nullptr) {
                return;
            }
            const auto* spec = data::find_mob_by_name(std::string(entry->type));
            if (spec == nullptr || spec->category != category) {
                return;
            }

            // The threat level is what a naturally spawned mob is created with; it is
            // computed here so the equipment work has a single source for it.
            if (category == data::MobCategory::Monster) {
                (void)threat_level(difficulty_, game_time, local_ticks(cx, cz),
                    moon_brightness_);
            }

            const int pack = entry->min_count +
                static_cast<int>(random(rng) % static_cast<std::uint32_t>(
                    entry->max_count - entry->min_count + 1));

            for (int i = 0; i < pack && count < limit; ++i) {
                const double x = static_cast<double>(start_x) + 0.5 +
                    static_cast<double>(static_cast<int>(random(rng) % 7)) - 3.0;
                const double z = static_cast<double>(start_z) + 0.5 +
                    static_cast<double>(static_cast<int>(random(rng) % 7)) - 3.0;
                if (near_player_or_spawn(players, x, z)) {
                    continue;
                }
                const bool baby = category != data::MobCategory::Monster &&
                    (random(rng) % 100) < 5;
                if (place(mobs, *spec, x, z, baby, night, rng)) {
                    ++count;
                }
            }
        }

        bool place_any(MobManager& mobs, double x, double z, bool night, std::uint32_t& rng) {
            const int biome = world_.biome_index
                ? world_.biome_index(static_cast<std::int32_t>(std::floor(x)),
                    static_cast<std::int32_t>(std::floor(z)))
                : -1;
            const auto* list = list_for(biome);
            if (list == nullptr) {
                return false;
            }

            // The initial pass fills the surface first, which is what a starting world needs;
            // the monster rule of the cycle keeps it populated afterwards. A flooded column
            // is offered to the water categories instead, so an ocean starts with fish.
            const bool water = world_.is_water && world_.is_water(
                static_cast<std::int32_t>(std::floor(x)),
                static_cast<std::int32_t>(std::floor(z)));
            const auto category = water
                ? ((random(rng) % 100) < 60 ? data::MobCategory::WaterCreature
                    : data::MobCategory::WaterAmbient)
                : (random(rng) % 100) < 30
                ? data::MobCategory::Monster
                : (random(rng) % 2) == 0 ? data::MobCategory::Creature
                : data::MobCategory::Ambient;
            const auto* entry = pick(list, category, rng);
            if (entry == nullptr) {
                return false;
            }
            const auto* spec = data::find_mob_by_name(std::string(entry->type));
            if (spec == nullptr || spec->category != category) {
                return false;
            }
            if (count_category(mobs, category) >=
                static_cast<std::size_t>(max_instances(category))) {
                return false;
            }
            return place(mobs, *spec, x, z, false, night, rng);
        }

        const data::BiomeSpawn* pick(const data::BiomeSpawnList* list,
            data::MobCategory category, std::uint32_t& rng) const {
            int total = 0;
            for (int i = 0; i < list->count; ++i) {
                const auto& entry = data::kBiomeSpawns[list->begin + i];
                if (category_from(entry.category) != category) {
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
                if (category_from(entry.category) != category) {
                    continue;
                }
                roll -= entry.weight;
                if (roll < 0) {
                    return &entry;
                }
            }
            return nullptr;
        }

        // One spawn: the position, the placement rule, the light rule, then the mob.
        bool place(MobManager& mobs, const data::MobSpec& spec, double x, double z, bool baby,
            bool night, std::uint32_t& rng) {
            const auto bx = static_cast<std::int32_t>(std::floor(x));
            const auto bz = static_cast<std::int32_t>(std::floor(z));
            const int y = spawn_y(spec, bx, bz, rng);
            if (!place_allowed(spec, bx, y, bz)) {
                return false;
            }
            if (!light_allowed(spec, bx, y, bz, night)) {
                return false;
            }
            return mobs.spawn_at(std::string(spec.id), x, static_cast<double>(y), z, baby,
                false);
        }

        // The block a mob would stand in. Monsters and cave water sample below the surface,
        // which is what populates caves; everything else starts from the height map.
        int spawn_y(const data::MobSpec& spec, std::int32_t x, std::int32_t z,
            std::uint32_t& rng) const {
            const int surface = world_.surface_y ? world_.surface_y(x, z) : 64;

            if (spec.category == data::MobCategory::UndergroundWaterCreature) {
                const int floor = world_.min_y + 1;
                const int top = std::min(kUndergroundWaterMaxY, surface);
                const int span = std::max(1, top - floor);
                return floor + static_cast<int>(random(rng) % static_cast<std::uint32_t>(span));
            }

            if (spec.placement == data::SpawnPlacement::InWater) {
                int y = std::min(surface - 1, world_.sea_level);
                for (int i = 0; i < kWaterFloorSearch; ++i, --y) {
                    if (kind_at(x, y, z) == SpawnBlock::Water) {
                        return y;
                    }
                }
                return y;
            }

            int y = surface;
            if (spec.category == data::MobCategory::Monster && (random(rng) % 100) < 50) {
                const int floor = world_.min_y + 1;
                const int span = std::max(1, surface - floor);
                y = floor + static_cast<int>(random(rng) % static_cast<std::uint32_t>(span));
            }

            for (int step = 0; step < kMaxStepUp; ++step) {
                const SpawnBlock kind = kind_at(x, y, z);
                if (kind == SpawnBlock::Air || kind == SpawnBlock::Water) {
                    break;
                }
                ++y;
            }
            return y;
        }

        static constexpr int kMaxStepUp = 4;

        SpawnBlock kind_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            if (world_.block_kind) {
                return world_.block_kind(x, y, z);
            }
            const int surface = world_.surface_y ? world_.surface_y(x, z) : 64;
            if (world_.is_water && world_.is_water(x, z)) {
                return y <= surface ? SpawnBlock::Water : SpawnBlock::Air;
            }
            if (y < surface) {
                return y == surface - 1 ? SpawnBlock::Soil : SpawnBlock::Solid;
            }
            return SpawnBlock::Air;
        }

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
                if (spec.category == data::MobCategory::UndergroundWaterCreature) {
                    if (y > kUndergroundWaterMaxY) {
                        return false;
                    }
                    // Underground means under a roof: open water is the surface category's.
                    const SpawnBlock roof = kind_at(x, y + 3, z);
                    if (roof != SpawnBlock::Solid && roof != SpawnBlock::Soil) {
                        return false;
                    }
                }
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
                // The animals tag: an animal stands on a surface it can graze, not on stone.
                if (spec.light != data::SpawnLight::Dark && below != SpawnBlock::Soil) {
                    return false;
                }
                return true;
            }
            case data::SpawnPlacement::NoRestrictions:
            default:
                // Nothing is required of the position, but nothing lives inside a fluid
                // either; the species still has to stand on something that is not water.
                return here == SpawnBlock::Air && below != SpawnBlock::Water;
            }
        }

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
                return block == 0 && brightness < 8;
            }
            return brightness > 8;
        }

        SpawnWorld world_;
        Difficulty difficulty_ = Difficulty::Normal;
        std::int32_t spawn_x_ = 0;
        std::int32_t spawn_z_ = 0;
        bool has_spawn_point_ = false;
        float moon_brightness_ = 0.0f;
        std::size_t chunk_cursor_ = 0;
        std::unordered_map<std::uint64_t, std::int64_t> local_time_;
    };

} // namespace terila::entity
