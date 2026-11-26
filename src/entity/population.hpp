#pragma once

// The natural population of the world: who lives where, and how much of it the world may
// hold. A cycle offers the loaded chunks around the players to the categories that still
// have room, the biome's own weighted list picks a species at a random spot in the chunk,
// and that species' placement and light rules decide whether it may stand there. The caps
// scale with the area that is actually spawnable, and nothing spawns in peaceful.

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

    // What the cycle needs from the world; the server wires these to its generator and its
    // chunk cache. Nothing here reaches into chunks.
    struct SpawnWorld {
        std::function<int(std::int32_t x, std::int32_t z)> biome_index = {};
        std::function<int(std::int32_t x, std::int32_t z)> surface_y = {};
        // One above the topmost non air block of a column: the height a spawn position is
        // drawn under, which the game takes from its surface heightmap.
        std::function<int(std::int32_t x, std::int32_t z)> height_at = {};
        std::function<bool(std::int32_t x, std::int32_t z)> is_water = {};
        // Stored light of a position, or -1 when that column is not loaded. An unloaded
        // column is never spawned in, which is what keeps the cycle off terrain nobody has.
        std::function<int(std::int32_t x, std::int32_t y, std::int32_t z)> block_light = {};
        std::function<int(std::int32_t x, std::int32_t y, std::int32_t z)> sky_light = {};
        std::function<SpawnBlock(std::int32_t x, std::int32_t y, std::int32_t z)> block_kind = {};
        // A state with a collision box, for the box a mob of its size would occupy.
        std::function<bool(std::int32_t x, std::int32_t y, std::int32_t z)> collides = {};
        // The game's animals tag: what an animal may stand on.
        std::function<bool(std::int32_t x, std::int32_t y, std::int32_t z)> animals_spawnable = {};
        std::int32_t min_y = -64;
        std::int32_t sea_level = 63;
        // The dimension type's darkness window for monsters: the raw brightness it compares
        // against, and the block light above which nothing hostile appears.
        int monster_spawn_light_min = 0;
        int monster_spawn_light_max = 7;
        int monster_spawn_block_light_limit = 0;
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

        // Level#getSkyDarken of the moment: the sky light the sun does not deliver any more.
        // The server hands over the clock's own value; without it the cycle falls back to
        // the day/night flag it is called with.
        void set_sky_darkening(int darkening) { sky_darkening_ = darkening; }

        // The population a fresh world starts with. The cycle below does the work; this walks
        // a wider disc once so the first player does not meet an empty world. The disc is
        // centred on the first player and every spot keeps the distance the cycle keeps, so
        // nothing appears on the player who caused the pass.
        std::size_t initial(MobManager& mobs, const std::vector<PlayerPos>& players,
            double radius, std::uint32_t& rng, bool night) {
            if (players.empty()) {
                return 0;
            }
            sky_darkening_ = sky_darkening_ >= 0 ? sky_darkening_ : (night ? kNightSkyDarken : 0);
            const PlayerPos& anchor = players.front();
            std::size_t spawned = 0;
            for (int attempt = 0; attempt < kInitialAttempts; ++attempt) {
                const double angle = static_cast<double>(random(rng) % 360) *
                    3.14159265358979323846 / 180.0;
                const double distance = radius * static_cast<double>(random(rng) % 1000) / 1000.0;
                const double x = anchor.x + std::cos(angle) * distance;
                const double z = anchor.z + std::sin(angle) * distance;
                if (place_any(mobs, players, x, z, rng)) {
                    ++spawned;
                }
            }
            return spawned;
        }

        // One tick of the cycle. The chunk list only changes when a player crosses a chunk
        // border, capacities are derived from its size, and only a rotating window of chunks
        // is visited so the cost per tick stays flat no matter how far the players have
        // explored.
        void tick(MobManager& mobs, const std::vector<PlayerPos>& players, bool night,
            std::uint32_t& rng, std::int64_t game_time) {
            if (players.empty()) {
                return;
            }
            if (!refresh_chunks(players)) {
                return;
            }
            sky_darkening_ = sky_darkening_ >= 0 ? sky_darkening_
                                                 : (night ? kNightSkyDarken : 0);

            std::array<std::size_t, 8> counts{};
            count_categories(mobs, counts);
            count_local(mobs, players);

            // The room a category has: its per-chunk maximum scaled to the spawnable area.
            // 289 is 17 squared, the area the maximum is written for.
            const auto capacity = [size = chunks_.size()](data::MobCategory category) {
                const int per_chunk = max_instances(category);
                if (per_chunk <= 0) {
                    return std::size_t{ 0 };
                }
                return static_cast<std::size_t>(per_chunk) * size / kMagicNumber;
            };

            const std::size_t window = std::min<std::size_t>(kChunksPerTick, chunks_.size());
            for (std::size_t i = 0; i < window; ++i) {
                const auto [cx, cz] = chunks_[(chunk_cursor_ + i) % chunks_.size()];
                for (const auto category : kCategories) {
                    if (difficulty_ == Difficulty::Peaceful &&
                        category == data::MobCategory::Monster) {
                        continue;
                    }
                    const auto slot = static_cast<std::size_t>(category);
                    if (counts[slot] >= capacity(category)) {
                        continue;
                    }
                    if (!can_spawn_locally(cx, cz, category)) {
                        continue;
                    }
                    spawn_chunk(mobs, players, cx, cz, category, rng, counts[slot],
                        capacity(category), game_time);
                }
            }
            chunk_cursor_ = (chunk_cursor_ + window) % chunks_.size();
            age_local_ticks(players);
            bound_local_time(chunk_set_);
        }

        // Built chunk lists since the process started, so a test can see that a stationary
        // group of players pays for the list once instead of once a tick.
        std::size_t chunk_list_builds() const noexcept { return chunk_list_builds_; }

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
        static constexpr int kMagicNumber = 17 * 17;
        // The local time only matters near players, so the map is trimmed back to the chunks
        // that are being visited once it grows past this.
        static constexpr std::size_t kMaxTrackedChunks = 4096;
        static constexpr std::size_t kChunksPerTick = 32;
        // The starting population is a convenience: the cycle fills the world around the
        // players anyway, so this only has to put something in front of the first join.
        static constexpr int kInitialAttempts = 160;
        static constexpr double kMinPlayerDistance = 24.0;
        static constexpr double kMinSpawnPointDistance = 24.0;
        static constexpr int kNightSkyDarken = 11;
        static constexpr int kMaxSpawnClusterSize = 4;
        static constexpr int kUndergroundWaterMaxY = 30;
        static constexpr int kWaterFloorSearch = 12;
        static constexpr int kGroupsPerChunk = 3;
        static constexpr int kGroupDrift = 6;

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

        static int random_between(std::uint32_t& rng, int low, int high) {
            if (high <= low) {
                return low;
            }
            return low + static_cast<int>(random(rng) %
                static_cast<std::uint32_t>(high - low + 1));
        }

        static float random_float(std::uint32_t& rng) {
            return static_cast<float>(random(rng) & 0x00FFFFFFu) / 16777216.0f;
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

        static constexpr std::size_t kBiomeCount =
            static_cast<std::size_t>(world::terrain::kBiomeCount);

        // The biome table is fixed for the run, so the linear search over its names is paid
        // once per biome instead of once per spawn attempt.
        const data::BiomeSpawnList* list_for(int biome) {
            if (biome < 0 || biome >= static_cast<int>(kBiomeCount)) {
                return nullptr;
            }
            if (!biome_lists_ready_) {
                for (std::size_t i = 0; i < kBiomeCount; ++i) {
                    biome_lists_[i] =
                        data::find_biome_spawns(world::terrain::kBiomeNames[i]);
                }
                biome_lists_ready_ = true;
            }
            return biome_lists_[static_cast<std::size_t>(biome)];
        }

        static std::size_t count_category(const MobManager& mobs, data::MobCategory category) {
            return static_cast<std::size_t>(std::count_if(mobs.mobs().begin(), mobs.mobs().end(),
                [category](const Mob& mob) {
                    return !mob.dead && mob.category() == category;
                }));
        }

        // Rebuilds the spawnable chunk list only when a player changed chunk. Both used to
        // cost a full rebuild and a player loop per mob, every tick.
        bool refresh_chunks(const std::vector<PlayerPos>& players) {
            std::vector<std::pair<std::int32_t, std::int32_t>> centers;
            centers.reserve(players.size());
            for (const auto& player : players) {
                centers.emplace_back(
                    static_cast<std::int32_t>(std::floor(player.x)) >> 4,
                    static_cast<std::int32_t>(std::floor(player.z)) >> 4);
            }
            if (centers == player_chunks_ && !chunks_.empty()) {
                return true;
            }

            player_chunks_ = std::move(centers);
            chunks_.clear();
            chunk_set_.clear();
            for (const auto& [center_x, center_z] : player_chunks_) {
                for (int dz = -kSpawnChunkRadius; dz <= kSpawnChunkRadius; ++dz) {
                    for (int dx = -kSpawnChunkRadius; dx <= kSpawnChunkRadius; ++dx) {
                        const std::int32_t cx = center_x + dx;
                        const std::int32_t cz = center_z + dz;
                        if (chunk_set_.insert(chunk_key(cx, cz)).second) {
                            chunks_.emplace_back(cx, cz);
                        }
                    }
                }
            }
            ++chunk_list_builds_;
            if (!chunks_.empty()) {
                chunk_cursor_ %= chunks_.size();
            }
            return !chunks_.empty();
        }

        void count_categories(const MobManager& mobs, std::array<std::size_t, 8>& counts) const {
            // Counted over the chunk area the cycle spawns in, not a circular radius: a mob
            // that walked to the corner of that area still occupies its slot.
            for (const auto& mob : mobs.mobs()) {
                if (mob.dead || mob.persistent) {
                    continue;
                }
                const auto mob_cx = static_cast<std::int32_t>(std::floor(mob.x)) >> 4;
                const auto mob_cz = static_cast<std::int32_t>(std::floor(mob.z)) >> 4;
                if (chunk_set_.count(chunk_key(mob_cx, mob_cz)) != 0) {
                    ++counts[static_cast<std::size_t>(mob.category())];
                }
            }
        }

        // The per player count the local cap is written against: a player who is standing in
        // a crowd has no room left around them, no matter what the global count says.
        void count_local(const MobManager& mobs, const std::vector<PlayerPos>& players) {
            local_counts_.assign(players.size(), std::array<std::size_t, 8>{});
            for (std::size_t index = 0; index < players.size(); ++index) {
                const auto& player = players[index];
                for (const auto& mob : mobs.mobs()) {
                    if (mob.dead || mob.persistent) {
                        continue;
                    }
                    const double dx = mob.x - player.x;
                    const double dz = mob.z - player.z;
                    if (dx * dx + dz * dz > kPlayerSpawnRange * kPlayerSpawnRange) {
                        continue;
                    }
                    ++local_counts_[index][static_cast<std::size_t>(mob.category())];
                }
            }
        }

        bool can_spawn_locally(std::int32_t cx, std::int32_t cz,
            data::MobCategory category) const {
            const int per_chunk = max_instances(category);
            if (per_chunk <= 0) {
                return false;
            }
            for (std::size_t index = 0; index < player_chunks_.size(); ++index) {
                const auto [player_cx, player_cz] = player_chunks_[index];
                if (std::abs(player_cx - cx) > kSpawnChunkRadius ||
                    std::abs(player_cz - cz) > kSpawnChunkRadius) {
                    continue;
                }
                if (index >= local_counts_.size() ||
                    local_counts_[index][static_cast<std::size_t>(category)] <
                        static_cast<std::size_t>(per_chunk)) {
                    return true;
                }
            }
            return false;
        }

        static double distance_squared(double ax, double az, double bx, double bz) {
            const double dx = ax - bx;
            const double dz = az - bz;
            return dx * dx + dz * dz;
        }

        // A world can be explored for days; the counter per chunk must not follow it forever.
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

        // One chunk of one category: a random spot in the column, then up to three groups of
        // drifting positions around it. Every position re-picks the species, which is what
        // makes a herd one species and the next group another.
        void spawn_chunk(MobManager& mobs, const std::vector<PlayerPos>& players,
            std::int32_t cx, std::int32_t cz, data::MobCategory category, std::uint32_t& rng,
            std::size_t& count, std::size_t limit, std::int64_t game_time) {
            const std::int32_t start_x = cx * 16 + random_between(rng, 0, 15);
            const std::int32_t start_z = cz * 16 + random_between(rng, 0, 15);
            const int top = height_at(start_x, start_z);
            const int start_y = random_between(rng, world_.min_y, std::max(world_.min_y, top));
            if (start_y < world_.min_y + 1) {
                return;
            }
            // A full block at the spot is a redstone conductor: nothing is placed inside one.
            if (kind_at(start_x, start_y, start_z) == SpawnBlock::Solid) {
                return;
            }

            int cluster = 0;
            for (int group = 0; group < kGroupsPerChunk; ++group) {
                std::int32_t x = start_x;
                std::int32_t z = start_z;
                const data::BiomeSpawn* entry = nullptr;
                const data::MobSpec* spec = nullptr;
                int pack = 0;
                int placed = 0;
                const int positions = static_cast<int>(
                    std::ceil(random_float(rng) * static_cast<float>(kMaxSpawnClusterSize)));

                for (int i = 0; i < positions; ++i) {
                    x += random_between(rng, 0, kGroupDrift - 1) -
                        random_between(rng, 0, kGroupDrift - 1);
                    z += random_between(rng, 0, kGroupDrift - 1) -
                        random_between(rng, 0, kGroupDrift - 1);
                    if (!far_from_players_and_spawn(players, x, start_y, z)) {
                        continue;
                    }
                    if (chunk_set_.count(chunk_key(x >> 4, z >> 4)) == 0) {
                        continue;
                    }

                    if (entry == nullptr) {
                        const int biome = world_.biome_index ? world_.biome_index(x, z) : -1;
                        const auto* list = list_for(biome);
                        if (list == nullptr) {
                            break;
                        }
                        entry = pick(list, category, rng);
                        if (entry == nullptr) {
                            break;
                        }
                        spec = data::find_mob_by_name(std::string(entry->type));
                        if (spec == nullptr || spec->category != category) {
                            break;
                        }
                        pack = random_between(rng, entry->min_count, entry->max_count);
                        if (category == data::MobCategory::Monster) {
                            (void)threat_level(difficulty_, game_time,
                                local_ticks(cx, cz), moon_brightness_);
                        }
                    }

                    if (!place_at(mobs, *spec, x, start_y, z, rng)) {
                        continue;
                    }
                    ++cluster;
                    ++placed;
                    ++count;
                    if (cluster >= kMaxSpawnClusterSize || count >= limit) {
                        return;
                    }
                    if (placed >= pack) {
                        break;
                    }
                }
            }
        }

        // NaturalSpawner.isRightDistanceToPlayerAndSpawnPoint: outside the reach of every
        // player and outside the world's own spawn area.
        bool far_from_players_and_spawn(const std::vector<PlayerPos>& players, std::int32_t x,
            std::int32_t y, std::int32_t z) const {
            for (const auto& player : players) {
                const double dx = static_cast<double>(x) + 0.5 - player.x;
                const double dy = static_cast<double>(y) - player.y;
                const double dz = static_cast<double>(z) + 0.5 - player.z;
                if (dx * dx + dy * dy + dz * dz <=
                    kMinPlayerDistance * kMinPlayerDistance) {
                    return false;
                }
            }
            if (!has_spawn_point_) {
                return true;
            }
            return distance_squared(static_cast<double>(x) + 0.5, static_cast<double>(z) + 0.5,
                static_cast<double>(spawn_x_), static_cast<double>(spawn_z_)) >=
                kMinSpawnPointDistance * kMinSpawnPointDistance;
        }

        // The population a fresh world starts with. The biome's own list decides which
        // category a spot is offered to, and the same placement rules apply as in the cycle,
        // so nothing appears inside a hill or on a block its species cannot stand on.
        bool place_any(MobManager& mobs, const std::vector<PlayerPos>& players, double x,
            double z, std::uint32_t& rng) {
            const auto bx = static_cast<std::int32_t>(std::floor(x));
            const auto bz = static_cast<std::int32_t>(std::floor(z));
            const auto* list = list_for(
                world_.biome_index ? world_.biome_index(bx, bz) : -1);
            if (list == nullptr) {
                return false;
            }

            // The starting pass fills the surface first, which is what a starting world needs;
            // a flooded column is offered to the water categories instead, so an ocean starts
            // with fish.
            const bool water = world_.is_water && world_.is_water(bx, bz);
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

            const int y = spawn_y(*spec, bx, bz, rng);
            if (!far_from_players_and_spawn(players, bx, y, bz)) {
                return false;
            }
            return place_at(mobs, *spec, bx, y, bz, rng);
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

        // One spawn: the position, the placement rule, the light rule, the box the mob would
        // fill, and only then the mob.
        bool place_at(MobManager& mobs, const data::MobSpec& spec, std::int32_t x,
            std::int32_t y, std::int32_t z, std::uint32_t& rng) {
            if (!placement_ok(spec, x, y, z)) {
                return false;
            }
            if (!light_ok(spec, x, y, z, rng)) {
                return false;
            }
            if (!box_clear(spec, x, y, z)) {
                return false;
            }
            return mobs.spawn_at(std::string(spec.id), static_cast<double>(x) + 0.5,
                static_cast<double>(y), static_cast<double>(z) + 0.5, false, false);
        }

        // SpawnPlacements: on ground wants the block below to own the spawn and both cells the
        // mob fills to be empty; in water wants the fluid itself and no full block above it.
        bool placement_ok(const data::MobSpec& spec, std::int32_t x, std::int32_t y,
            std::int32_t z) {
            switch (spec.placement) {
            case data::SpawnPlacement::InWater:
                return kind_at(x, y, z) == SpawnBlock::Water &&
                    kind_at(x, y + 1, z) != SpawnBlock::Solid;
            case data::SpawnPlacement::InLava:
                return false;
            case data::SpawnPlacement::OnGround: {
                const SpawnBlock below = kind_at(x, y - 1, z);
                if (below != SpawnBlock::Solid && below != SpawnBlock::Soil) {
                    return false;
                }
                return empty_spawn_block(x, y, z) && empty_spawn_block(x, y + 1, z);
            }
            case data::SpawnPlacement::NoRestrictions:
            default:
                return empty_spawn_block(x, y, z);
            }
        }

        // NaturalSpawner.isValidEmptySpawnBlock: no full collision box, no fluid, and not a
        // block the game forbids spawning inside.
        SpawnBlock kind_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            if (world_.block_kind) {
                return world_.block_kind(x, y, z);
            }
            const int surface = world_.surface_y ? world_.surface_y(x, z) : 64;
            if (y < surface) {
                return y == surface - 1 ? SpawnBlock::Soil : SpawnBlock::Solid;
            }
            return SpawnBlock::Air;
        }

        bool empty_spawn_block(std::int32_t x, std::int32_t y, std::int32_t z) const {
            const SpawnBlock kind = kind_at(x, y, z);
            return kind == SpawnBlock::Air || kind == SpawnBlock::Plant;
        }

        int height_at(std::int32_t x, std::int32_t z) const {
            if (world_.height_at) {
                return world_.height_at(x, z);
            }
            if (world_.surface_y) {
                return world_.surface_y(x, z);
            }
            return world_.sea_level + 1;
        }

        // The light test of a species. A monster reads the stored sky and block light the way
        // the game does: the sky alone may already rule it out, then the block light limit,
        // and the brightness left after the sun's darkening is compared to a random value
        // from the dimension's window. An animal only asks for a raw brightness above eight.
        bool light_ok(const data::MobSpec& spec, std::int32_t x, std::int32_t y,
            std::int32_t z, std::uint32_t& rng) const {
            if (spec.light == data::SpawnLight::Any) {
                return true;
            }
            const int sky = world_.sky_light ? world_.sky_light(x, y, z) : 15;
            const int block = world_.block_light ? world_.block_light(x, y, z) : 0;
            if (sky < 0 || block < 0) {
                return false;
            }
            // GlowSquid.checkGlowSquidSpawnRules: the one underground water creature of the
            // overworld only appears in dark water well below the sea.
            if (spec.category == data::MobCategory::UndergroundWaterCreature) {
                if (y > world_.sea_level - 33) {
                    return false;
                }
                if (std::max(sky, block) != 0) {
                    return false;
                }
                return kind_at(x, y, z) == SpawnBlock::Water;
            }
            if (spec.light == data::SpawnLight::Bright) {
                if (world_.animals_spawnable) {
                    if (!world_.animals_spawnable(x, y - 1, z)) {
                        return false;
                    }
                }
                else if (kind_at(x, y - 1, z) != SpawnBlock::Soil) {
                    return false;
                }
                return std::max(sky, block) > 8;
            }

            // Monster.isDarkEnoughToSpawn.
            if (sky > static_cast<int>(random(rng) % 32u)) {
                return false;
            }
            if (world_.monster_spawn_block_light_limit < 15 &&
                block > world_.monster_spawn_block_light_limit) {
                return false;
            }
            const int brightness = std::max(block, sky - sky_darkening_);
            const int window = random_between(rng, world_.monster_spawn_light_min,
                world_.monster_spawn_light_max);
            return brightness <= window;
        }

        // noCollision of the box a mob of this size would occupy, in whole blocks.
        bool box_clear(const data::MobSpec& spec, std::int32_t x, std::int32_t y,
            std::int32_t z) const {
            const double half = static_cast<double>(spec.width) * 0.5;
            const double center_x = static_cast<double>(x) + 0.5;
            const double center_z = static_cast<double>(z) + 0.5;
            const int min_x = static_cast<int>(std::floor(center_x - half));
            const int max_x = static_cast<int>(std::ceil(center_x + half)) - 1;
            const int min_z = static_cast<int>(std::floor(center_z - half));
            const int max_z = static_cast<int>(std::ceil(center_z + half)) - 1;
            const int max_y = static_cast<int>(std::ceil(
                static_cast<double>(y) + static_cast<double>(spec.height))) - 1;

            for (int by = y; by <= max_y; ++by) {
                for (int bx = min_x; bx <= max_x; ++bx) {
                    for (int bz = min_z; bz <= max_z; ++bz) {
                        const bool solid = world_.collides
                            ? world_.collides(bx, by, bz)
                            : kind_at(bx, by, bz) == SpawnBlock::Solid;
                        if (solid) {
                            return false;
                        }
                    }
                }
            }
            return true;
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
        static constexpr double kPlayerSpawnRange = 128.0;

        SpawnWorld world_;
        Difficulty difficulty_ = Difficulty::Normal;
        std::int32_t spawn_x_ = 0;
        std::int32_t spawn_z_ = 0;
        bool has_spawn_point_ = false;
        float moon_brightness_ = 0.0f;
        int sky_darkening_ = -1;
        std::size_t chunk_cursor_ = 0;
        std::size_t chunk_list_builds_ = 0;
        std::vector<std::pair<std::int32_t, std::int32_t>> player_chunks_;
        std::vector<std::pair<std::int32_t, std::int32_t>> chunks_;
        std::unordered_set<std::uint64_t> chunk_set_;
        std::array<const data::BiomeSpawnList*, kBiomeCount> biome_lists_{};
        bool biome_lists_ready_ = false;
        std::unordered_map<std::uint64_t, std::int64_t> local_time_;
        std::vector<std::array<std::size_t, 8>> local_counts_;
    };

} // namespace terila::entity
