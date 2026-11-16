#include "world/block_break.hpp"

#include <gtest/gtest.h>

#include <limits>

using terila::world::break_ticks;

TEST(BlockBreak, HandBreaksASoftBlockInSecondsOfHardness) {
    // Dirt is 0.5 and needs no tool: 0.75 s, the time the game itself gives it.
    EXPECT_EQ(break_ticks(0.5f, false), 15);
    // Planks are 2.0 by hand -> 3 s.
    EXPECT_EQ(break_ticks(2.0f, false), 60);
}

TEST(BlockBreak, MissingTheToolItWantsTakesMuchLonger) {
    // Stone is 1.5 and wants a pickaxe: 7.5 s without one, 2.25 s if it did not.
    EXPECT_EQ(break_ticks(1.5f, true), 150);
    EXPECT_EQ(break_ticks(1.5f, false), 45);
    EXPECT_GT(break_ticks(1.5f, true), break_ticks(1.5f, false));
}

TEST(BlockBreak, ZeroResistanceIsInstant) {
    EXPECT_EQ(break_ticks(0.0f, false), 0);
    EXPECT_EQ(break_ticks(0.0f, true), 0);
}

TEST(BlockBreak, NegativeHardnessNeverYields) {
    EXPECT_EQ(break_ticks(-1.0f, false), -1);
}

TEST(BlockBreak, ANonsenseHardnessNeverYieldsEither) {
    // NaN would make every comparison false and slip through as an instant break.
    EXPECT_EQ(break_ticks(std::numeric_limits<float>::quiet_NaN(), false), -1);
}
