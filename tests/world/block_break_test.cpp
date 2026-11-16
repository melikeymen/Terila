#include "world/block_break.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <unordered_set>
#include <vector>

using terila::world::break_ticks;
using terila::world::ToolAgainstBlock;
using terila::world::tool_against;
using terila::world::ToolRule;

namespace {

    // The rules a diamond pickaxe carries in items/minecraft.json: slow on what it cannot
    // harvest, full speed on everything the pickaxe tag covers.
    std::vector<ToolRule> diamond_pickaxe() {
        return {
            ToolRule{ "#minecraft:incorrect_for_diamond_tool", false, 1.0f },
            ToolRule{ "#minecraft:mineable/pickaxe", true, 8.0f },
        };
    }

    std::vector<ToolRule> wooden_pickaxe() {
        return {
            ToolRule{ "#minecraft:incorrect_for_wooden_tool", false, 1.0f },
            ToolRule{ "#minecraft:mineable/pickaxe", true, 2.0f },
        };
    }

} // namespace

TEST(BlockBreak, EmptyHandTakesSecondsOfHardness) {
    // Dirt is 0.5 and needs no tool: 0.75 s, the time the game itself gives it.
    EXPECT_EQ(break_ticks(0.5f, true, 1.0f), 15);
    // Stone is 1.5 and refuses a hand entirely: 7.5 s.
    EXPECT_EQ(break_ticks(1.5f, false, 1.0f), 150);
}

TEST(BlockBreak, TheRightToolDividesTheTimeByItsSpeed) {
    const std::unordered_set<std::string> stone{ "minecraft:mineable/pickaxe" };
    const ToolAgainstBlock hit = tool_against(diamond_pickaxe(), "minecraft:stone", &stone, true);

    EXPECT_TRUE(hit.can_harvest);
    EXPECT_FLOAT_EQ(hit.speed, 8.0f);
    // 1.5 hardness, 8× speed: six ticks, which is the 0.3 s the game shows for a diamond
    // pickaxe on stone.
    EXPECT_EQ(break_ticks(1.5f, hit.can_harvest, hit.speed), 6);
}

TEST(BlockBreak, AToolBelowTheBlocksTierFallsBackToBareHandPace) {
    // Diamond ore asks for iron, so a wooden pickaxe matches the "incorrect" rule first.
    const std::unordered_set<std::string> diamond_ore{
        "minecraft:mineable/pickaxe", "minecraft:incorrect_for_wooden_tool" };
    const ToolAgainstBlock hit = tool_against(wooden_pickaxe(), "minecraft:diamond_ore", &diamond_ore, true);

    EXPECT_FALSE(hit.can_harvest);
    EXPECT_FLOAT_EQ(hit.speed, 1.0f);
    EXPECT_EQ(break_ticks(3.0f, hit.can_harvest, hit.speed), 300);
}

TEST(BlockBreak, AToolGoodAtSomethingElseLeavesTheBlockAlone) {
    // Dirt is shovel work; a pickaxe matched no rule, so it digs like a hand and dirt still
    // gives itself up because it asks for no particular tool.
    const std::unordered_set<std::string> dirt{ "minecraft:mineable/shovel" };
    const ToolAgainstBlock hit = tool_against(diamond_pickaxe(), "minecraft:dirt", &dirt, false);

    EXPECT_TRUE(hit.can_harvest);
    EXPECT_FLOAT_EQ(hit.speed, 1.0f);
    EXPECT_EQ(break_ticks(0.5f, hit.can_harvest, hit.speed), 15);
}

TEST(BlockBreak, ABlockWithNoTagsStillMatchesSingleBlockRules) {
    const std::vector<ToolRule> shears{ ToolRule{ "minecraft:cobweb", true, 15.0f } };

    const ToolAgainstBlock hit = tool_against(shears, "minecraft:cobweb", nullptr, false);

    EXPECT_TRUE(hit.can_harvest);
    EXPECT_FLOAT_EQ(hit.speed, 15.0f);
}

TEST(BlockBreak, NonsenseToolSpeedNeverSpeedsTheDigUp) {
    EXPECT_EQ(break_ticks(1.0f, true, 0.5f), 30);
    EXPECT_EQ(break_ticks(1.0f, true, -3.0f), 30);
}

TEST(BlockBreak, ZeroResistanceIsInstant) {
    EXPECT_EQ(break_ticks(0.0f, false, 1.0f), 0);
    EXPECT_EQ(break_ticks(0.0f, true, 8.0f), 0);
}

TEST(BlockBreak, NegativeHardnessNeverYields) {
    EXPECT_EQ(break_ticks(-1.0f, true, 8.0f), -1);
}

TEST(BlockBreak, ANonsenseHardnessNeverYieldsEither) {
    // NaN would make every comparison false and slip through as an instant break.
    EXPECT_EQ(break_ticks(std::numeric_limits<float>::quiet_NaN(), true, 1.0f), -1);
}
