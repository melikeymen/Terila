#include <gtest/gtest.h>

#include "commands/command_parse.hpp"

using terila::commands::parse_execute;

TEST(CommandParseTest, RunsACommandAsTheCaller) {
    const auto parsed = parse_execute("run give minecraft:stone 3");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->selector.empty());
    EXPECT_EQ(parsed->command, "give minecraft:stone 3");
}

TEST(CommandParseTest, AsTakesASelectorBeforeRun) {
    const auto parsed = parse_execute("as @a run give minecraft:stone");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->selector, "@a");
    EXPECT_EQ(parsed->command, "give minecraft:stone");
}

TEST(CommandParseTest, AsTakesAPlayerName) {
    const auto parsed = parse_execute("as Steve run give minecraft:bread 2");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->selector, "steve");   // names match without regard to case
    EXPECT_EQ(parsed->command, "give minecraft:bread 2");
}

TEST(CommandParseTest, NestedCommandsStayIntact) {
    const auto parsed = parse_execute("as @s run execute as @a run give stone");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->selector, "@s");
    EXPECT_EQ(parsed->command, "execute as @a run give stone");
}

TEST(CommandParseTest, KeepsTheCommandTextAsTyped) {
    const auto parsed = parse_execute("run give minecraft:stone[minecraft:custom_name='\"Big\"'] 1");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->command,
        "give minecraft:stone[minecraft:custom_name='\"Big\"'] 1");
}

TEST(CommandParseTest, RejectsAMissingRun) {
    EXPECT_FALSE(parse_execute("as @a").has_value());
    EXPECT_FALSE(parse_execute("as @a give stone").has_value());
    EXPECT_FALSE(parse_execute("as").has_value());
}

TEST(CommandParseTest, RejectsAMissingCommand) {
    EXPECT_FALSE(parse_execute("").has_value());
    EXPECT_FALSE(parse_execute("run").has_value());
    EXPECT_FALSE(parse_execute("   run   ").has_value());
    EXPECT_FALSE(parse_execute("execute").has_value());
    EXPECT_FALSE(parse_execute("positioned 0 0 0 run give stone").has_value());
}

TEST(CommandParseTest, IgnoresExtraWhitespace) {
    const auto parsed = parse_execute("  as   @p   run    give   stone  ");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->selector, "@p");
    EXPECT_EQ(parsed->command, "give   stone");
}
