#include <gtest/gtest.h>

#include "world/falling_blocks.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace world = terila::world;

namespace {

    constexpr std::int32_t kAir = 0;
    constexpr std::int32_t kStone = 1;
    constexpr std::int32_t kSand = 2;
    constexpr std::int32_t kGravel = 3;
    constexpr std::int32_t kPlant = 4;
    constexpr std::int32_t kWater = 5;

    // A world with a stone floor and air above it. Everything the simulation moves lands in
    // the overlay, so a test reads the same way it writes: one block at a time.
    struct Sim {
        world::BlockWorld world;
        world::FallingBlocks falling;
        std::vector<std::pair<world::BlockPos, std::int32_t>> changes;
        std::int32_t floor_y = 0;
        std::int32_t min_y = -64;
        std::int32_t max_y = 319;

        explicit Sim(std::int32_t floor = 0, std::int32_t min = -64,
            std::int32_t max = 319)
            : floor_y(floor), min_y(min), max_y(max) {
            falling.set_world(&world, kAir, min_y, max_y);
            falling.set_states({ kSand, kGravel }, { kAir, kWater, kPlant });
            falling.set_state_at([this](const world::BlockPos& pos) {
                return generated(pos);
                });
            falling.set_change_sink([this](const world::BlockPos& pos, std::int32_t state) {
                changes.push_back({ pos, state });
                });
        }

        std::int32_t generated(const world::BlockPos& pos) const {
            return pos.y <= floor_y ? kStone : kAir;
        }

        void set(std::int32_t x, std::int32_t y, std::int32_t z, std::int32_t state) {
            world.set(world::BlockPos{ x, y, z }, state);
        }

        std::int32_t at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            return world.get(world::BlockPos{ x, y, z }, generated({ x, y, z }));
        }

        void ticks(int count) {
            for (int i = 0; i < count; ++i) {
                falling.tick();
            }
        }
    };

} // namespace

TEST(FallingBlockKindTest, VanillaFallingBlockKinds) {
    EXPECT_TRUE(world::is_falling_block("minecraft:sand"));
    EXPECT_TRUE(world::is_falling_block("minecraft:red_sand"));
    EXPECT_TRUE(world::is_falling_block("minecraft:gravel"));
    EXPECT_TRUE(world::is_falling_block("minecraft:white_concrete_powder"));
    EXPECT_TRUE(world::is_falling_block("minecraft:damaged_anvil"));
    // Sandstone is not sand, and the brushable ones do not fall.
    EXPECT_FALSE(world::is_falling_block("minecraft:sandstone"));
    EXPECT_FALSE(world::is_falling_block("minecraft:suspicious_sand"));
}

TEST(FallingBlocksTest, SandFallsToTheGround) {
    Sim sim;
    sim.set(0, 5, 0, kSand);
    sim.falling.block_changed({ 0, 5, 0 });

    // Vanilla schedules the block's tick two ticks out.
    sim.ticks(2);
    EXPECT_EQ(sim.at(0, 5, 0), kSand);
    EXPECT_TRUE(sim.changes.empty());

    sim.ticks(1);
    EXPECT_EQ(sim.at(0, 5, 0), kAir);
    EXPECT_EQ(sim.at(0, 4, 0), kSand);

    sim.ticks(10);
    EXPECT_EQ(sim.at(0, 1, 0), kSand);
    EXPECT_EQ(sim.at(0, 4, 0), kAir);
    EXPECT_EQ(sim.falling.pending(), 0u);
}

TEST(FallingBlocksTest, SupportedBlockStays) {
    Sim sim;
    sim.set(0, 1, 0, kSand);
    sim.falling.block_changed({ 0, 1, 0 });
    sim.ticks(6);

    EXPECT_EQ(sim.at(0, 1, 0), kSand);
    EXPECT_EQ(sim.falling.pending(), 0u);
    EXPECT_TRUE(sim.changes.empty());
}

TEST(FallingBlocksTest, AStackFollowsItselfDown) {
    Sim sim;
    sim.set(0, 1, 0, kSand);
    sim.set(0, 2, 0, kSand);
    sim.set(0, 3, 0, kSand);

    // Digging the cell the bottom block stands on: only the cell above the hole notices.
    sim.set(0, 1, 0, kAir);
    sim.falling.block_changed({ 0, 1, 0 });
    sim.ticks(30);

    EXPECT_EQ(sim.at(0, 1, 0), kSand);
    EXPECT_EQ(sim.at(0, 2, 0), kSand);
    EXPECT_EQ(sim.at(0, 3, 0), kAir);
    EXPECT_EQ(sim.falling.pending(), 0u);
}

TEST(FallingBlocksTest, FallsThroughWaterAndPlants) {
    Sim sim;
    sim.set(0, 3, 0, kPlant);
    sim.set(0, 4, 0, kWater);
    sim.set(0, 5, 0, kGravel);
    sim.falling.block_changed({ 0, 5, 0 });
    sim.ticks(20);

    EXPECT_EQ(sim.at(0, 1, 0), kGravel);
    EXPECT_EQ(sim.at(0, 3, 0), kAir);
    EXPECT_EQ(sim.at(0, 4, 0), kAir);
}

TEST(FallingBlocksTest, BlocksThatDoNotFallAreNotSimulated) {
    Sim sim;
    sim.set(0, 5, 0, kStone);
    sim.falling.block_changed({ 0, 5, 0 });
    sim.ticks(5);

    EXPECT_EQ(sim.at(0, 5, 0), kStone);
    EXPECT_EQ(sim.falling.pending(), 0u);
    EXPECT_TRUE(sim.changes.empty());
}

TEST(FallingBlocksTest, TheBottomOfTheWorldHoldsIt) {
    // A floor far below the world: the block can only fall to min_y.
    Sim sim(-128);
    sim.set(0, -60, 0, kSand);
    sim.falling.block_changed({ 0, -60, 0 });
    sim.ticks(20);

    EXPECT_EQ(sim.at(0, -64, 0), kSand);
    EXPECT_EQ(sim.falling.pending(), 0u);
}

TEST(FallingBlocksTest, ABlockRemovedBeforeItsTickDoesNothing) {
    Sim sim;
    sim.set(0, 5, 0, kSand);
    sim.falling.block_changed({ 0, 5, 0 });
    sim.set(0, 5, 0, kAir);

    sim.ticks(6);
    EXPECT_TRUE(sim.changes.empty());
    EXPECT_EQ(sim.falling.pending(), 0u);
}

TEST(FallingBlocksTest, OnlyChangedCellsAreReported) {
    Sim sim;
    sim.set(0, 5, 0, kSand);
    sim.falling.block_changed({ 0, 5, 0 });
    sim.ticks(3);

    ASSERT_EQ(sim.changes.size(), 2u);
    EXPECT_EQ(sim.changes[0].first.y, 5);
    EXPECT_EQ(sim.changes[0].second, kAir);
    EXPECT_EQ(sim.changes[1].first.y, 4);
    EXPECT_EQ(sim.changes[1].second, kSand);
}
