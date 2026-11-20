#include <gtest/gtest.h>

#include "entity/mob.hpp"
#include "entity/population.hpp"
#include "world/world_clock.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace entity = terila::entity;
namespace data = terila::entity::data;
namespace world = terila::world;

namespace {

    // Flat ground at y=64 with no water anywhere.
    void make_manager(entity::MobManager& mobs) {
        mobs.set_ground_resolver([](double, double) { return 64.0; });
        mobs.set_water_resolver([](double, double) { return false; });
    }

    entity::SpawnWorld flat_world(int biome,
        entity::SpawnBlock ground = entity::SpawnBlock::Soil, bool water = false) {
        entity::SpawnWorld world_view;
        world_view.biome_index = [biome](std::int32_t, std::int32_t) { return biome; };
        world_view.surface_y = [](std::int32_t, std::int32_t) { return 64; };
        world_view.is_water = [water](std::int32_t, std::int32_t) { return water; };
        world_view.block_light = [](std::int32_t, std::int32_t, std::int32_t) { return 0; };
        world_view.sky_light = [](std::int32_t, std::int32_t, std::int32_t) { return 15; };
        world_view.block_kind = [ground, water](std::int32_t, std::int32_t y, std::int32_t) {
            if (water) {
                return y <= 64 ? entity::SpawnBlock::Water : entity::SpawnBlock::Air;
            }
            if (y < 63) return entity::SpawnBlock::Solid;
            if (y == 63) return ground;
            return entity::SpawnBlock::Air;
            };
        return world_view;
    }

} // namespace

TEST(MobDataTest, AttributesComeFromTheGameData) {
    EXPECT_GT(std::size(data::kMobs), 80u);
    EXPECT_GT(std::size(data::kGoals), 400u);

    const auto* cow = data::find_mob("minecraft:cow");
    ASSERT_NE(cow, nullptr);
    EXPECT_FLOAT_EQ(cow->max_health, 10.0f);
    EXPECT_FLOAT_EQ(cow->movement_speed, 0.2f);
    EXPECT_EQ(cow->category, data::MobCategory::Creature);

    const auto* zombie = data::find_mob_by_name("zombie");
    ASSERT_NE(zombie, nullptr);
    EXPECT_EQ(std::string(zombie->id), "minecraft:zombie");
    EXPECT_FLOAT_EQ(zombie->max_health, 20.0f);
    EXPECT_FLOAT_EQ(zombie->attack_damage, 3.0f);
    EXPECT_FLOAT_EQ(zombie->follow_range, 35.0f);
    EXPECT_FLOAT_EQ(zombie->armor, 2.0f);
    EXPECT_EQ(zombie->category, data::MobCategory::Monster);

    EXPECT_EQ(data::find_mob("minecraft:not_a_mob"), nullptr);
}

TEST(MobDataTest, GoalsAreRegisteredWithVanillaPriorities) {
    const auto* zombie = data::find_mob("minecraft:zombie");
    ASSERT_NE(zombie, nullptr);
    ASSERT_GT(zombie->goal_count, 0);

    bool melee = false;
    int previous = -1;
    for (int i = 0; i < zombie->goal_count; ++i) {
        const auto& goal = data::kGoals[zombie->goal_begin + i];
        if (goal.kind == "MeleeAttackGoal" || goal.kind == "ZombieAttackGoal") {
            melee = true;
        }
        EXPECT_GE(goal.priority, previous) << "goals are stored in priority order";
        previous = goal.priority;
    }
    EXPECT_TRUE(melee) << "a zombie walks up to its target";
}

TEST(BiomeSpawnTest, ListsAreTheGamesOwnSpawns) {
    const auto* plains = data::find_biome_spawns("minecraft:plains");
    ASSERT_NE(plains, nullptr);
    EXPECT_GT(plains->count, 0);
    EXPECT_EQ(data::find_biome_spawns("minecraft:not_a_biome"), nullptr);

    bool cow = false;
    bool zombie = false;
    for (int i = 0; i < plains->count; ++i) {
        const auto& entry = data::kBiomeSpawns[plains->begin + i];
        EXPECT_GT(entry.weight, 0);
        EXPECT_GE(entry.max_count, entry.min_count);
        if (entry.type == "minecraft:cow") cow = true;
        if (entry.type == "minecraft:zombie") zombie = true;
    }
    EXPECT_TRUE(cow);
    EXPECT_TRUE(zombie);
    EXPECT_EQ(entity::Population::max_instances(data::MobCategory::Creature), 10);
    EXPECT_EQ(entity::Population::max_instances(data::MobCategory::Monster), 70);
    EXPECT_EQ(entity::Population::max_instances(data::MobCategory::WaterAmbient), 20);
    EXPECT_EQ(entity::Population::max_instances(data::MobCategory::WaterCreature), 5);
    EXPECT_EQ(entity::Population::max_instances(data::MobCategory::Misc), -1);
}

TEST(MobTest, SpawnUsesTheSpecAndStandsOnTheGround) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 10.0, 20.0));
    EXPECT_FALSE(mobs.spawn("minecraft:not_a_mob", 0.0, 0.0));

    ASSERT_EQ(mobs.mobs().size(), 1u);
    const auto& cow = mobs.mobs().front();
    EXPECT_EQ(cow.type_name, "minecraft:cow");
    EXPECT_DOUBLE_EQ(cow.health, 10.0);
    EXPECT_DOUBLE_EQ(cow.y, 64.0);
    EXPECT_TRUE(cow.on_ground);
}

TEST(MobTest, AMobWalksWhereItIsLooking) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 0.0, 0.0));

    // Entity.getInputVector turns yaw 0 into +Z, 90 into -X and 180 into -Z. A mob that
    // walks in the direction it reports has to move along that vector every single tick.
    for (int tick = 0; tick < 240; ++tick) {
        const auto before = mobs.mobs().front();
        mobs.tick({});
        const auto& cow = mobs.mobs().front();
        const double dx = cow.x - before.x;
        const double dz = cow.z - before.z;
        if (std::abs(dx) + std::abs(dz) < 1e-9) {
            continue;
        }
        constexpr double kPi = 3.14159265358979323846;
        const double radians = static_cast<double>(cow.yaw) * kPi / 180.0;
        const double facing_x = -std::sin(radians);
        const double facing_z = std::cos(radians);
        const double length = std::sqrt(dx * dx + dz * dz);
        const double dot = (dx / length) * facing_x + (dz / length) * facing_z;
        EXPECT_GT(dot, 0.999) << "the mob walks along its yaw, not away from it";
    }
}

TEST(MobTest, AStrollingMobWalksAndStaysOnTheGround) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 0.0, 0.0));
    const double start_x = mobs.mobs().front().x;
    const double start_z = mobs.mobs().front().z;

    for (int tick = 0; tick < 400; ++tick) {
        mobs.tick({});
    }

    const auto& cow = mobs.mobs().front();
    const double moved = std::sqrt((cow.x - start_x) * (cow.x - start_x) +
        (cow.z - start_z) * (cow.z - start_z));
    EXPECT_GT(moved, 1.0) << "a cow wanders";
    EXPECT_DOUBLE_EQ(cow.y, 64.0) << "gravity keeps it on the surface";
    EXPECT_LE(moved, 400.0 * 0.2 + 1.0) << "and never faster than its movement speed";
}

TEST(MobTest, AMonsterChasesAPlayerInRange) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:zombie", 0.0, 0.0));

    const std::vector<entity::PlayerPos> players = { {0.0, 64.0, 12.0, 7} };
    const double start = std::abs(mobs.mobs().front().z - 12.0);
    for (int tick = 0; tick < 200; ++tick) {
        mobs.tick(players);
    }

    const auto& zombie = mobs.mobs().front();
    const double now = std::sqrt(zombie.x * zombie.x + (zombie.z - 12.0) * (zombie.z - 12.0));
    EXPECT_LT(now, start) << "the zombie closes the distance";
    EXPECT_EQ(zombie.goal, entity::GoalKind::Melee) << "and it does so with the melee goal";
}

TEST(MobTest, AMeleeHitReachesThePlayerItLandedOn) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:zombie", 0.0, 0.0));

    const std::vector<entity::PlayerPos> players = { {0.0, 64.0, 1.0, 42} };
    bool landed = false;
    for (int tick = 0; tick < 60 && !landed; ++tick) {
        mobs.tick(players);
        for (const auto& hit : mobs.hits()) {
            EXPECT_EQ(hit.player, 42) << "the hit names the player it landed on";
            EXPECT_DOUBLE_EQ(hit.damage, 3.0) << "a zombie hits for 3";
            EXPECT_FALSE(hit.ranged);
            EXPECT_EQ(hit.attacker, "minecraft:zombie");
            landed = true;
        }
    }
    EXPECT_TRUE(landed) << "a zombie standing next to a player hits it";

    // Creative players are not a target's worth of damage.
    entity::PlayerPos creative = players.front();
    creative.creative = true;
    bool hit_creative = false;
    for (int tick = 0; tick < 60; ++tick) {
        mobs.tick({ creative });
        hit_creative = hit_creative || !mobs.hits().empty();
    }
    EXPECT_FALSE(hit_creative);
}

TEST(MobTest, ADistantPlayerIsIgnored) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:zombie", 0.0, 0.0));

    const std::vector<entity::PlayerPos> players = { {0.0, 64.0, 200.0, 7} };
    for (int tick = 0; tick < 120; ++tick) {
        mobs.tick(players);
    }
    EXPECT_NE(mobs.mobs().front().goal, entity::GoalKind::Melee)
        << "35 blocks of follow range is the zombie's limit";
}

TEST(MobTest, AFedMobFallsInLoveAndTwoOfThemMakeABaby) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 0.0, 0.0));
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 4.0, 0.0));
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 40.0, 0.0));

    const std::int32_t first = mobs.mobs()[0].id;
    const std::int32_t second = mobs.mobs()[1].id;
    EXPECT_FALSE(mobs.feed(first, "minecraft:stone")) << "a cow does not eat stone";
    EXPECT_TRUE(mobs.feed(first, "minecraft:wheat"));
    EXPECT_TRUE(mobs.feed(second, "minecraft:wheat"));
    EXPECT_FALSE(mobs.feed(first, "minecraft:wheat")) << "already in love";

    for (int tick = 0; tick < 200; ++tick) {
        mobs.tick({});
    }

    ASSERT_EQ(mobs.mobs().size(), 4u) << "a third cow was born";
    const auto& baby = *std::find_if(mobs.mobs().begin(), mobs.mobs().end(),
        [](const entity::Mob& mob) { return mob.baby; });
    EXPECT_EQ(baby.type_name, "minecraft:cow");
    EXPECT_LT(baby.scale(), 1.0);
    for (const auto& mob : mobs.mobs()) {
        if (mob.id == first || mob.id == second) {
            EXPECT_FALSE(mob.in_love()) << "the parents left love behind";
        }
    }
}

TEST(MobTest, ABabyGrowsUpAndFollowsAnAdult) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 0.0, 0.0));
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 6.0, 0.0, true));

    const auto& baby = mobs.mobs()[1];
    ASSERT_TRUE(baby.baby);
    EXPECT_EQ(baby.goal, entity::GoalKind::Unsupported) << "nothing runs before the first tick";

    // FollowParentGoal looks for the closest adult within eight blocks, so the baby starts
    // well inside that range and closes the rest of the gap.
    const auto gap = [&mobs] {
        const auto& young = mobs.mobs()[1];
        const auto& adult = mobs.mobs()[0];
        return std::sqrt((young.x - adult.x) * (young.x - adult.x) +
            (young.z - adult.z) * (young.z - adult.z));
    };
    const double start = gap();
    for (int tick = 0; tick < 60; ++tick) {
        mobs.tick({});
    }
    EXPECT_EQ(mobs.mobs()[1].goal, entity::GoalKind::FollowParent);
    EXPECT_LT(gap(), start) << "FollowParent closes the gap";

    // Ten feedings take a tenth of the childhood each, so it is an adult afterwards.
    for (int feeding = 0; feeding < 100; ++feeding) {
        mobs.feed(mobs.mobs()[1].id, "minecraft:wheat");
    }
    EXPECT_FALSE(mobs.mobs()[1].baby);
}

TEST(MobTest, APlayerHoldingFoodTemptsTheMob) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 0.0, 0.0));

    entity::PlayerPos player;
    player.z = 6.0;
    player.id = 7;
    player.held_item = "minecraft:wheat";

    for (int tick = 0; tick < 80; ++tick) {
        mobs.tick({ player });
    }
    const auto& cow = mobs.mobs().front();
    EXPECT_EQ(cow.goal, entity::GoalKind::Tempt);
    EXPECT_LT(std::abs(cow.z - 6.0), 2.0) << "the cow walks up to the wheat";

    // A player holding nothing is not tempting.
    entity::PlayerPos empty;
    empty.z = 6.0;
    empty.id = 7;
    for (int tick = 0; tick < 40; ++tick) {
        mobs.tick({ empty });
    }
    EXPECT_NE(mobs.mobs().front().goal, entity::GoalKind::Tempt);
}

TEST(WorldClockTest, TheDayCycleDecidesWhenItIsNight) {
    terila::world::WorldClock clock;
    clock.set_time_of_day(1000);
    EXPECT_FALSE(clock.night()) << "morning";

    clock.set_time_of_day(terila::world::WorldClock::kNoon);
    EXPECT_FALSE(clock.night());

    clock.set_time_of_day(13000);
    EXPECT_TRUE(clock.night()) << "dusk is dark enough for monsters";

    clock.set_time_of_day(18000);
    EXPECT_TRUE(clock.night());

    clock.set_time_of_day(23000);
    EXPECT_FALSE(clock.night()) << "sunrise";

    clock.set_time_of_day(0);
    clock.tick(24000);
    EXPECT_EQ(clock.time_of_day, 0) << "a full day wraps around";
    EXPECT_EQ(clock.game_time, 24000);
}

TEST(MobTest, ArmorSoaksPartOfTheHitAndDeathRemovesTheMob) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:zombie", 0.0, 0.0));

    const std::int32_t id = mobs.mobs().front().id;
    ASSERT_TRUE(mobs.damage(id, 4, 0.0f));
    // Armour 2 takes 0.4 points off a 4 damage hit: 4 * (1 - 0.4/25).
    EXPECT_NEAR(mobs.mobs().front().health, 20.0 - 4.0 * (1.0 - 0.4 / 25.0), 0.001);

    ASSERT_TRUE(mobs.damage(id, 100, 0.0f));
    EXPECT_TRUE(mobs.mobs().front().dead);
    EXPECT_DOUBLE_EQ(mobs.mobs().front().health, 0.0);

    for (int tick = 0; tick < 25; ++tick) {
        mobs.tick({});
    }
    EXPECT_TRUE(mobs.mobs().empty()) << "a dead mob leaves after its death animation";
}

TEST(PopulationTest, TheInitialPopulationHonoursTheCategoryCaps) {
    entity::MobManager mobs;
    make_manager(mobs);

    entity::Population spawner;
    spawner.set_world(flat_world(world::terrain::kPlains));
    spawner.set_difficulty(entity::Difficulty::Normal);

    std::uint32_t rng = 0x1234567u;
    const std::size_t spawned = spawner.initial(mobs, 0, 0, 48.0, rng, false);
    EXPECT_GT(spawned, 0u);

    std::size_t creatures = 0;
    std::size_t monsters = 0;
    for (const auto& mob : mobs.mobs()) {
        ASSERT_NE(mob.spec, nullptr);
        EXPECT_FALSE(mob.persistent) << "a natural mob is not a kept mob";
        if (mob.category() == data::MobCategory::Creature) ++creatures;
        if (mob.category() == data::MobCategory::Monster) ++monsters;
    }
    // A day-time initial pass cannot place monsters; the cap still has to hold at night.
    EXPECT_EQ(monsters, 0u) << "nothing hostile appears in the daylight";
    EXPECT_LE(creatures,
        static_cast<std::size_t>(entity::Population::max_instances(data::MobCategory::Creature)));
    EXPECT_GT(creatures, 0u) << "plains lists cows and sheep";
}

TEST(PopulationTest, AnimalsNeedGrassAndStayOutOfTheWater) {
    // Sand is a solid spawn surface, but it is not animals_spawnable_on: no cows on a beach.
    entity::MobManager on_sand;
    make_manager(on_sand);
    entity::Population sand_spawner;
    sand_spawner.set_world(flat_world(world::terrain::kPlains, entity::SpawnBlock::Solid));
    std::uint32_t sand_rng = 0x5A5A5Au;
    EXPECT_EQ(sand_spawner.initial(on_sand, 0, 0, 48.0, sand_rng, false), 0u)
        << "an animal wants the animals tag under it";

    // A flooded plains column has no ground to stand on, so nothing landbound spawns.
    entity::MobManager flooded;
    make_manager(flooded);
    entity::Population water_spawner;
    water_spawner.set_world(flat_world(world::terrain::kPlains, entity::SpawnBlock::Soil, true));
    std::uint32_t water_rng = 0x5A5A5Au;
    EXPECT_EQ(water_spawner.initial(flooded, 0, 0, 48.0, water_rng, true), 0u);
    for (int tick = 0; tick < 200; ++tick) {
        water_spawner.tick(flooded, { { 0.0, 64.0, 0.0, 1 } }, true, water_rng, 0);
    }
    EXPECT_TRUE(flooded.mobs().empty()) << "no land mob spawns inside water";
}

TEST(PopulationTest, WaterMobsOnlySpawnInWater) {
    // The ocean's list is squid and fish; their InWater placement rejects dry ground.
    entity::MobManager dry;
    make_manager(dry);
    entity::Population dry_spawner;
    dry_spawner.set_world(flat_world(world::terrain::kOcean));
    std::uint32_t dry_rng = 0xC0FFEEu;
    EXPECT_EQ(dry_spawner.initial(dry, 0, 0, 48.0, dry_rng, false), 0u);

    entity::MobManager wet;
    make_manager(wet);
    entity::Population wet_spawner;
    wet_spawner.set_world(flat_world(world::terrain::kOcean, entity::SpawnBlock::Solid, true));
    std::uint32_t wet_rng = 0xC0FFEEu;
    EXPECT_GT(wet_spawner.initial(wet, 0, 0, 48.0, wet_rng, false), 0u);
    for (const auto& mob : wet.mobs()) {
        const bool water = mob.category() == data::MobCategory::WaterCreature ||
            mob.category() == data::MobCategory::WaterAmbient ||
            mob.category() == data::MobCategory::UndergroundWaterCreature ||
            mob.category() == data::MobCategory::Axolotls;
        EXPECT_TRUE(water) << mob.type_name << " belongs in the water";
    }
}

TEST(PopulationTest, MonstersOnlyArriveAtNight) {
    const std::vector<entity::PlayerPos> players = { { 0.0, 64.0, 0.0, 1 } };

    entity::MobManager daytime;
    make_manager(daytime);
    entity::Population day_spawner;
    day_spawner.set_world(flat_world(world::terrain::kPlains));
    std::uint32_t day_rng = 0xABCDEFu;
    for (int tick = 0; tick < 400; ++tick) {
        day_spawner.tick(daytime, players, false, day_rng, 0);
    }
    for (const auto& mob : daytime.mobs()) {
        EXPECT_NE(mob.category(), data::MobCategory::Monster) << "no monsters in daylight";
    }

    entity::MobManager mobs;
    make_manager(mobs);
    entity::Population spawner;
    spawner.set_world(flat_world(world::terrain::kPlains));
    std::uint32_t night_rng = 0xABCDEFu;
    for (int tick = 0; tick < 400; ++tick) {
        spawner.tick(mobs, players, true, night_rng, 0);
    }
    // The cap is counted over the spawnable chunks; a mob that wandered out of that area is
    // still alive, so only the ones inside it are counted here.
    const auto in_area = [](const entity::Mob& mob) {
        return std::abs(mob.x) < 136.0 && std::abs(mob.z) < 136.0;
    };
    const auto monsters = std::count_if(mobs.mobs().begin(), mobs.mobs().end(),
        [&](const entity::Mob& mob) {
            return in_area(mob) && mob.category() == data::MobCategory::Monster;
        });
    EXPECT_GT(monsters, 0) << "the night brings monsters";
    EXPECT_LE(static_cast<std::size_t>(monsters),
        static_cast<std::size_t>(entity::Population::max_instances(data::MobCategory::Monster)));
}

TEST(PopulationTest, RuntimeSpawnsKeepTheirDistanceFromPlayers) {
    entity::MobManager mobs;
    make_manager(mobs);

    entity::Population spawner;
    spawner.set_world(flat_world(world::terrain::kPlains));

    std::uint32_t rng = 0x7654321u;
    const std::vector<entity::PlayerPos> players = { { 0.0, 64.0, 0.0, 1 } };
    for (int tick = 0; tick < 200; ++tick) {
        spawner.tick(mobs, players, true, rng, 0);
    }

    EXPECT_GT(mobs.mobs().size(), 0u);
    for (const auto& mob : mobs.mobs()) {
        const double distance = std::sqrt(mob.x * mob.x + mob.z * mob.z);
        // The group is at least 24 blocks out; a pack then spreads by up to 3 per axis.
        EXPECT_GE(distance, 24.0 - 5.0) << "nothing spawns next to a player";
    }
}

TEST(PopulationTest, PeacefulNeverSpawnsAMonster) {
    entity::MobManager mobs;
    make_manager(mobs);

    entity::Population spawner;
    spawner.set_world(flat_world(world::terrain::kPlains));
    spawner.set_difficulty(entity::Difficulty::Peaceful);

    std::uint32_t rng = 0x13572468u;
    const std::vector<entity::PlayerPos> players = { { 0.0, 64.0, 0.0, 1 } };
    for (int tick = 0; tick < 400; ++tick) {
        spawner.tick(mobs, players, true, rng, 0);
    }
    for (const auto& mob : mobs.mobs()) {
        EXPECT_NE(mob.category(), data::MobCategory::Monster)
            << "peaceful keeps the monsters away even at night";
    }
}

TEST(DifficultyTest, TheThreatLevelGrowsWithTimeAndMoon) {
    const auto peaceful = entity::threat_level(entity::Difficulty::Peaceful, 0, 0, 1.0f);
    EXPECT_FLOAT_EQ(peaceful.effective, 0.0f);
    EXPECT_FLOAT_EQ(peaceful.special_multiplier(), 0.0f);

    // A fresh normal world: 2 * 0.75.
    const auto fresh = entity::threat_level(entity::Difficulty::Normal, 0, 0, 0.0f);
    EXPECT_FLOAT_EQ(fresh.effective, 1.5f);
    EXPECT_FALSE(fresh.hard());
    EXPECT_FLOAT_EQ(fresh.special_multiplier(), 0.0f);

    // Hard starts higher but a new world is still not "hard" by the effective measure.
    const auto fresh_hard = entity::threat_level(entity::Difficulty::Hard, 0, 0, 0.0f);
    EXPECT_FLOAT_EQ(fresh_hard.effective, 2.25f);
    EXPECT_FALSE(fresh_hard.hard());

    // Everything maxed: 0.75 + 0.25 global + 1.25 local, times 3.
    const auto grown = entity::threat_level(entity::Difficulty::Hard, 72000 + 1440000,
        3600000, 1.0f);
    EXPECT_FLOAT_EQ(grown.effective, 6.75f);
    EXPECT_TRUE(grown.hard());
    EXPECT_TRUE(grown.harder_than(4.0f));
    EXPECT_FLOAT_EQ(grown.special_multiplier(), 1.0f);

    // Easy halves the local part: (0.75 + 0.25 + 0.5) * 1.
    const auto easy = entity::threat_level(entity::Difficulty::Easy, 72000 + 1440000,
        3600000, 1.0f);
    EXPECT_FLOAT_EQ(easy.effective, 1.5f);
}

TEST(DifficultyTest, PeacefulRemovesTheMonstersThatWereAlreadyThere) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:zombie", 0.0, 0.0));
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 6.0, 0.0));

    const std::vector<entity::PlayerPos> players = { { 0.0, 64.0, 2.0, 1 } };
    mobs.tick(players, entity::Difficulty::Peaceful);

    ASSERT_EQ(mobs.mobs().size(), 1u) << "the zombie is gone, the cow stays";
    EXPECT_EQ(mobs.mobs().front().type_name, "minecraft:cow");
}

TEST(MobTest, AFarMobDespawnsAndAPersistentOneDoesNot) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:zombie", 0.0, 0.0));
    ASSERT_TRUE(mobs.spawn_at("minecraft:zombie", 1000.0, 64.0, 1000.0));

    const std::vector<entity::PlayerPos> players = { { 0.0, 64.0, 200.0, 1 } };
    mobs.tick(players);

    ASSERT_EQ(mobs.mobs().size(), 1u) << "the natural zombie left, the summoned one stayed";
    EXPECT_TRUE(mobs.mobs().front().persistent);
}

TEST(DifficultyTest, DifficultyNamesRoundTrip) {
    using entity::Difficulty;
    EXPECT_EQ(entity::difficulty_from("peaceful"), Difficulty::Peaceful);
    EXPECT_EQ(entity::difficulty_from("easy"), Difficulty::Easy);
    EXPECT_EQ(entity::difficulty_from("normal"), Difficulty::Normal);
    EXPECT_EQ(entity::difficulty_from("hard"), Difficulty::Hard);
    EXPECT_FALSE(entity::difficulty_from("impossible").has_value());
    EXPECT_STREQ(entity::difficulty_name(Difficulty::Hard), "hard");
}

TEST(MobAiTest, MostGoalRegistrationsAreImplemented) {
    std::size_t total = 0;
    std::size_t mapped = 0;
    std::size_t ranged = 0;
    for (const auto& mob : data::kMobs) {
        for (int i = 0; i < mob.goal_count; ++i) {
            const auto kind = entity::goal_kind_from(data::kGoals[mob.goal_begin + i].kind);
            ++total;
            if (kind != entity::GoalKind::Unsupported) {
                ++mapped;
            }
            if (kind == entity::GoalKind::Ranged) {
                ++ranged;
            }
        }
    }
    ASSERT_GT(total, 400u);
    // The remainder is raids, taming, trading, boss spells and the nether/end families.
    EXPECT_GT(mapped * 100 / total, 60u) << mapped << " of " << total << " goal rows";
    EXPECT_GT(ranged, 0u);
}

TEST(MobAiTest, ASkeletonShootsThePlayerItSees) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:skeleton", 0.0, 0.0));

    const std::vector<entity::PlayerPos> players = { { 0.0, 64.0, 10.0, 9 } };
    bool shot = false;
    for (int tick = 0; tick < 120 && !shot; ++tick) {
        mobs.tick(players);
        for (const auto& hit : mobs.hits()) {
            if (hit.player != 9) {
                continue;
            }
            EXPECT_TRUE(hit.ranged) << "a skeleton's hit is an arrow";
            EXPECT_EQ(hit.attacker, "minecraft:skeleton");
            EXPECT_GT(hit.damage, 0.0);
            shot = true;
        }
    }
    EXPECT_TRUE(shot) << "the bow is added at runtime, so the AI supplies it";
    EXPECT_EQ(mobs.mobs().front().goal, entity::GoalKind::Ranged);
}

TEST(MobAiTest, ASkeletonKeepsItsDistance) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:skeleton", 0.0, 0.0));

    const std::vector<entity::PlayerPos> players = { { 0.0, 64.0, 2.0, 9 } };
    for (int tick = 0; tick < 80; ++tick) {
        mobs.tick(players);
    }
    const auto& skeleton = mobs.mobs().front();
    const double distance = std::sqrt(skeleton.x * skeleton.x + skeleton.z * skeleton.z);
    EXPECT_GT(distance, 3.0) << "RangedAttackGoal backs away from a target that is too close";
}

TEST(MobAiTest, ACreeperSwellsAndExplodes) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:creeper", 0.0, 0.0));

    const std::vector<entity::PlayerPos> players = { { 0.0, 64.0, 1.5, 11 } };
    double damage = 0.0;
    for (int tick = 0; tick < 80; ++tick) {
        mobs.tick(players);
        for (const auto& hit : mobs.hits()) {
            EXPECT_EQ(hit.player, 11);
            EXPECT_FALSE(hit.ranged);
            damage = std::max(damage, hit.damage);
        }
    }
    EXPECT_GT(damage, 4.0) << "the blast hits harder than a punch";
    EXPECT_TRUE(mobs.mobs().empty()) << "the creeper is gone after the explosion";
}

TEST(MobAiTest, ASlimeHopsAround) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:slime", 0.0, 0.0));

    double highest = 64.0;
    for (int tick = 0; tick < 120; ++tick) {
        mobs.tick({});
        highest = std::max(highest, mobs.mobs().front().y);
    }
    EXPECT_GT(highest, 64.05) << "KeepOnJumpingGoal keeps a slime in the air";
}

TEST(MobAiTest, UndeadBurnInTheOpenSunAndSeekShade) {
    entity::MobManager mobs;
    make_manager(mobs);
    mobs.set_shade_resolver([](double, double, double) { return false; });
    ASSERT_TRUE(mobs.spawn("minecraft:skeleton", 0.0, 0.0));

    for (int tick = 0; tick < 60; ++tick) {
        mobs.tick({}, entity::Difficulty::Normal, false);
    }
    EXPECT_LT(mobs.mobs().front().health, 20.0) << "daylight burns undead";

    // Under a roof, or at night, nothing burns.
    entity::MobManager sheltered;
    make_manager(sheltered);
    sheltered.set_shade_resolver([](double, double, double) { return true; });
    ASSERT_TRUE(sheltered.spawn("minecraft:skeleton", 0.0, 0.0));
    for (int tick = 0; tick < 60; ++tick) {
        sheltered.tick({}, entity::Difficulty::Normal, false);
    }
    EXPECT_DOUBLE_EQ(sheltered.mobs().front().health, 20.0);
}

TEST(MobAiTest, ACreativePlayerIsNotATarget) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:zombie", 0.0, 0.0));

    entity::PlayerPos creative;
    creative.z = 5.0;
    creative.id = 3;
    creative.creative = true;
    for (int tick = 0; tick < 40; ++tick) {
        mobs.tick({ creative });
        EXPECT_TRUE(mobs.hits().empty());
    }
    EXPECT_NE(mobs.mobs().front().goal, entity::GoalKind::Melee);
    EXPECT_EQ(mobs.mobs().front().target_id, 0);
}

TEST(MobAiTest, TheGoalSpeedModifierIsApplied) {
    entity::MobManager mobs;
    make_manager(mobs);
    ASSERT_TRUE(mobs.spawn("minecraft:cow", 0.0, 0.0));
    mobs.tick({});

    // The first runnable goal of a cow is its water-avoiding stroll, registered at 1.0.
    double expected = 1.0;
    for (int i = 0; i < mobs.mobs().front().spec->goal_count; ++i) {
        const auto& goal = data::kGoals[mobs.mobs().front().spec->goal_begin + i];
        if (entity::goal_kind_from(goal.kind) == mobs.mobs().front().goal) {
            expected = goal.speed;
            break;
        }
    }
    EXPECT_DOUBLE_EQ(mobs.mobs().front().goal_speed, expected);
}

TEST(PopulationTest, TheLocalTimeMapDoesNotGrowForever) {
    entity::MobManager mobs;
    make_manager(mobs);

    entity::Population spawner;
    spawner.set_world(flat_world(world::terrain::kPlains));

    std::uint32_t rng = 0x0BADF00Du;
    // A player walking east forever: every tick ages a new three by three patch of chunks.
    for (int step = 0; step < 600; ++step) {
        const std::vector<entity::PlayerPos> players = {
            { static_cast<double>(step) * 16.0 + 8.0, 64.0, 0.0, 1 } };
        spawner.tick(mobs, players, true, rng, 0);
    }

    EXPECT_GT(spawner.tracked_chunks(), 0u);
    EXPECT_LE(spawner.tracked_chunks(), 4096u + 9u)
        << "the per-chunk time map is trimmed once it passes its bound";
}

TEST(PopulationTest, TheChunkListIsOnlyRebuiltWhenAPlayerChangesChunk) {
    entity::MobManager mobs;
    make_manager(mobs);

    entity::Population spawner;
    spawner.set_world(flat_world(world::terrain::kPlains));

    std::uint32_t rng = 0xFEEDFACEu;
    const std::vector<entity::PlayerPos> still = { { 8.0, 64.0, 8.0, 1 } };
    for (int tick = 0; tick < 50; ++tick) {
        spawner.tick(mobs, still, true, rng, 0);
    }
    EXPECT_EQ(spawner.chunk_list_builds(), 1u)
        << "standing still does not rebuild the chunk list every tick";

    const std::vector<entity::PlayerPos> moved = { { 8.0 + 16.0, 64.0, 8.0, 1 } };
    spawner.tick(mobs, moved, true, rng, 0);
    EXPECT_EQ(spawner.chunk_list_builds(), 2u) << "crossing a chunk border rebuilds it";
}
