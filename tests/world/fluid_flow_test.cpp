#include <gtest/gtest.h>

#include "world/fluid_flow.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace world = terila::world;

namespace {

    constexpr std::int32_t kAir = 0;
    constexpr std::int32_t kStone = 1;
    constexpr std::int32_t kPlant = 2;
    constexpr std::int32_t kObsidian = 3;
    constexpr std::int32_t kCobblestone = 4;
    // Water occupies 10..25 and lava 40..55, one state per legacy level, the way the
    // registry numbers them.
    constexpr std::int32_t kWaterBase = 10;
    constexpr std::int32_t kLavaBase = 40;

    struct Sim {
        world::BlockWorld world;
        world::FluidFlow fluids;
        std::vector<std::pair<world::BlockPos, std::int32_t>> changes;
        std::int32_t floor_y = 0;

        explicit Sim(std::int32_t floor = 0) : floor_y(floor) {
            fluids.set_world(&world, kAir, -64, 319);

            world::FluidFlow::Fluid water;
            for (int level = 0; level < world::FluidFlow::kLevels; ++level) {
                water.states[static_cast<std::size_t>(level)] = kWaterBase + level;
            }
            water.drop_off = 1;
            water.slope_find = 4;
            water.tick_delay = 5;
            water.source_conversion = true;
            fluids.set_fluid(world::FluidFlow::Kind::Water, water);

            world::FluidFlow::Fluid lava;
            for (int level = 0; level < world::FluidFlow::kLevels; ++level) {
                lava.states[static_cast<std::size_t>(level)] = kLavaBase + level;
            }
            lava.drop_off = 2;
            lava.slope_find = 2;
            lava.tick_delay = 30;
            lava.source_conversion = false;
            fluids.set_fluid(world::FluidFlow::Kind::Lava, lava);

            fluids.set_replaceable({ kAir, kPlant, kWaterBase, kLavaBase });
            fluids.set_solids(kObsidian, kCobblestone, kStone);
            fluids.set_state_at([this](const world::BlockPos& pos) {
                return generated(pos);
                });
            fluids.set_change_sink([this](const world::BlockPos& pos, std::int32_t state) {
                changes.push_back({ pos, state });
                });
        }

        std::int32_t generated(const world::BlockPos& pos) const {
            return pos.y <= floor_y ? kStone : kAir;
        }

        static std::int32_t water(int level) { return kWaterBase + level; }
        static std::int32_t lava(int level) { return kLavaBase + level; }

        void set(std::int32_t x, std::int32_t y, std::int32_t z, std::int32_t state) {
            world.set(world::BlockPos{ x, y, z }, state);
        }

        std::int32_t at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            return world.get(world::BlockPos{ x, y, z }, generated({ x, y, z }));
        }

        void ticks(int count) {
            for (int i = 0; i < count; ++i) {
                fluids.tick();
            }
        }
    };

} // namespace

TEST(FluidFlowTest, FlowingWaterCarriesItsLevel) {
    Sim sim;
    sim.set(0, 1, 0, Sim::water(0));
    sim.fluids.block_changed({ 0, 1, 0 });

    // The tick delay is five game ticks before the source pushes out.
    sim.ticks(5);
    EXPECT_EQ(sim.at(1, 1, 0), Sim::water(1));
    EXPECT_EQ(sim.at(0, 1, 1), Sim::water(1));

    // One block per water tick, so the front is three blocks out after fifteen ticks.
    sim.ticks(10);
    EXPECT_EQ(sim.at(2, 1, 0), Sim::water(2));
    EXPECT_EQ(sim.at(3, 1, 0), Sim::water(3));
    EXPECT_EQ(sim.at(0, 1, 3), Sim::water(3));
}

TEST(FluidFlowTest, TheLevelStopsAtSeven) {
    Sim sim;
    sim.set(0, 1, 0, Sim::water(0));
    sim.fluids.block_changed({ 0, 1, 0 });

    sim.ticks(60);
    EXPECT_EQ(sim.at(7, 1, 0), Sim::water(7));
    // A level 7 flow cannot reach an eighth block.
    EXPECT_EQ(sim.at(8, 1, 0), kAir);
}

TEST(FluidFlowTest, FlowingWaterFallsStraightAway) {
    Sim sim(-5);
    sim.set(0, 1, 0, Sim::water(0));
    sim.fluids.block_changed({ 0, 1, 0 });

    sim.ticks(30);
    EXPECT_EQ(sim.at(0, 0, 0), Sim::water(8));
    EXPECT_EQ(sim.at(0, -3, 0), Sim::water(8));
    // The column stands all the way down to the floor.
    EXPECT_EQ(sim.at(0, -4, 0), Sim::water(8));
    EXPECT_EQ(sim.at(0, -5, 0), kStone);
}

TEST(FluidFlowTest, AFallingColumnSpreadsWhereItLands) {
    Sim sim(-2);
    // Walls around the source keep it a one wide shaft, so only the falling column is seen.
    sim.set(1, 1, 0, kStone);
    sim.set(-1, 1, 0, kStone);
    sim.set(0, 1, 1, kStone);
    sim.set(0, 1, -1, kStone);
    sim.set(0, 1, 0, Sim::water(0));
    sim.fluids.block_changed({ 0, 1, 0 });

    sim.ticks(40);
    EXPECT_EQ(sim.at(0, 0, 0), Sim::water(8));
    EXPECT_EQ(sim.at(0, -1, 0), Sim::water(8));
    // It lands on the floor and only then fans out along it as level 1.
    EXPECT_EQ(sim.at(1, -1, 0), Sim::water(1));
    EXPECT_EQ(sim.at(0, -2, 0), kStone);
}

TEST(FluidFlowTest, TwoSourcesMakeTheGapASource) {
    Sim sim;
    sim.set(0, 1, 0, Sim::water(0));
    sim.set(2, 1, 0, Sim::water(0));
    sim.fluids.block_changed({ 0, 1, 0 });
    sim.fluids.block_changed({ 2, 1, 0 });

    sim.ticks(20);
    EXPECT_EQ(sim.at(1, 1, 0), Sim::water(0));
}

TEST(FluidFlowTest, RemovingTheSourceDrainsTheFlow) {
    Sim sim;
    sim.set(0, 1, 0, Sim::water(0));
    sim.fluids.block_changed({ 0, 1, 0 });
    sim.ticks(40);
    ASSERT_EQ(sim.at(3, 1, 0), Sim::water(3));

    sim.set(0, 1, 0, kAir);
    sim.fluids.block_changed({ 0, 1, 0 });
    sim.ticks(400);

    EXPECT_EQ(sim.at(1, 1, 0), kAir);
    EXPECT_EQ(sim.at(3, 1, 0), kAir);
    EXPECT_EQ(sim.fluids.pending(), 0u);
}

TEST(FluidFlowTest, WaterRunsToTheNearestHole) {
    // A flat floor with a shaft two blocks to the east: the flow heads for it instead of
    // spreading evenly in every direction.
    Sim sim(-3);
    for (int x = -4; x <= 4; ++x) {
        for (int z = -4; z <= 4; ++z) {
            sim.set(x, 0, z, kStone);
        }
    }
    sim.set(2, 0, 0, kAir);
    sim.set(0, 1, 0, Sim::water(0));
    sim.fluids.block_changed({ 0, 1, 0 });

    sim.ticks(5);
    EXPECT_EQ(sim.at(1, 1, 0), Sim::water(1));
    EXPECT_EQ(sim.at(0, 1, 1), kAir);
    EXPECT_EQ(sim.at(-1, 1, 0), kAir);

    sim.ticks(35);
    EXPECT_EQ(sim.at(2, 0, 0), Sim::water(8));
    EXPECT_EQ(sim.at(2, -2, 0), Sim::water(8));
}

TEST(FluidFlowTest, LavaStepsByTwo) {
    // Lava loses two levels per block, so a source reaches three blocks out and no further.
    Sim sim;
    sim.set(0, 1, 0, Sim::lava(0));
    sim.fluids.block_changed({ 0, 1, 0 });

    sim.ticks(30);
    EXPECT_EQ(sim.at(1, 1, 0), Sim::lava(2));

    sim.ticks(90);
    EXPECT_EQ(sim.at(2, 1, 0), Sim::lava(4));
    EXPECT_EQ(sim.at(3, 1, 0), Sim::lava(6));
    EXPECT_EQ(sim.at(4, 1, 0), kAir);
}

TEST(FluidFlowTest, LavaTouchingWaterSolidifies) {
    Sim sim;
    sim.set(1, 1, 0, Sim::lava(2));
    sim.set(2, 1, 0, Sim::water(0));
    sim.fluids.block_changed({ 1, 1, 0 });
    sim.fluids.block_changed({ 2, 1, 0 });

    sim.ticks(30);
    // A flowing lava cell next to water turns to cobblestone, a source to obsidian.
    EXPECT_EQ(sim.at(1, 1, 0), kCobblestone);

    sim.set(4, 1, 0, Sim::water(0));
    sim.set(5, 1, 0, Sim::lava(0));
    sim.fluids.block_changed({ 4, 1, 0 });
    sim.fluids.block_changed({ 5, 1, 0 });
    sim.ticks(30);
    EXPECT_EQ(sim.at(5, 1, 0), kObsidian);
}

TEST(FluidFlowTest, LavaPouredOntoWaterLeavesStone) {
    Sim sim(-1);
    sim.set(0, 0, 0, Sim::water(0));
    sim.set(0, 1, 0, Sim::lava(0));
    sim.fluids.block_changed({ 0, 0, 0 });
    sim.fluids.block_changed({ 0, 1, 0 });

    sim.ticks(30);
    EXPECT_EQ(sim.at(0, 0, 0), kStone);
    EXPECT_EQ(sim.at(0, 1, 0), Sim::lava(0));
}

TEST(FluidFlowTest, BucketsPlaceAndTakeSources) {
    Sim sim;
    EXPECT_TRUE(sim.fluids.place_source({ 0, 1, 0 }, world::FluidFlow::Kind::Water));
    EXPECT_EQ(sim.at(0, 1, 0), Sim::water(0));
    // A cell that cannot hold fluid refuses the pour.
    EXPECT_FALSE(sim.fluids.place_source({ 0, 0, 0 }, world::FluidFlow::Kind::Lava));

    world::FluidFlow::Kind removed{};
    EXPECT_TRUE(sim.fluids.remove_source({ 0, 1, 0 }, removed));
    EXPECT_EQ(removed, world::FluidFlow::Kind::Water);
    EXPECT_EQ(sim.at(0, 1, 0), kAir);

    // Flowing fluid is not a source, so a bucket leaves it alone.
    sim.set(2, 1, 0, Sim::water(3));
    EXPECT_FALSE(sim.fluids.remove_source({ 2, 1, 0 }, removed));
    EXPECT_EQ(sim.at(2, 1, 0), Sim::water(3));
}

TEST(FluidFlowTest, LevelsFollowTheFluidAmounts) {
    EXPECT_EQ(world::FluidFlow::amount_of(0), 8);
    EXPECT_EQ(world::FluidFlow::amount_of(1), 7);
    EXPECT_EQ(world::FluidFlow::amount_of(7), 1);
    EXPECT_EQ(world::FluidFlow::amount_of(8), 8);
    EXPECT_EQ(world::FluidFlow::amount_of(15), 1);
    EXPECT_TRUE(world::FluidFlow::is_falling(8));
    EXPECT_FALSE(world::FluidFlow::is_falling(7));

    Sim sim;
    EXPECT_EQ(sim.fluids.kind_of(Sim::water(4)), 0);
    EXPECT_EQ(sim.fluids.kind_of(Sim::lava(9)), 1);
    EXPECT_EQ(sim.fluids.kind_of(kStone), -1);
    EXPECT_EQ(sim.fluids.level_of(Sim::water(0)), 0);
    EXPECT_TRUE(sim.fluids.is_source(Sim::lava(0)));
    EXPECT_FALSE(sim.fluids.is_source(Sim::lava(1)));
}

TEST(FluidFlowTest, FluidStatesInTheTagDoNotRewriteThemselves) {
    Sim sim;
    // The washed-away tag lists the fluids themselves, so a flow must not treat its own
    // cells as empty ground and keep rewriting them.
    sim.fluids.set_replaceable({ kAir, kPlant, Sim::water(0), Sim::water(1), Sim::water(5),
        Sim::lava(0) });
    sim.set(0, 1, 0, Sim::water(0));
    sim.fluids.block_changed({ 0, 1, 0 });

    sim.ticks(60);
    EXPECT_EQ(sim.at(7, 1, 0), Sim::water(7));
    EXPECT_EQ(sim.at(8, 1, 0), kAir);
    EXPECT_EQ(sim.fluids.pending(), 0u);
}
