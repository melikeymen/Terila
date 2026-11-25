#include <gtest/gtest.h>

#include "commands/arguments/selector.hpp"

#include <cstdint>
#include <string>
#include <vector>

using terila::commands::parse;
using terila::commands::resolve;
using terila::commands::Sort;
using terila::commands::Target;

namespace {

    std::vector<Target> players() {
        return {
            Target{ 1, "Alex", 0.0, 64.0, 0.0 },
            Target{ 2, "Steve", 10.0, 64.0, 0.0 },
            Target{ 3, "Herobrine", 100.0, 64.0, 0.0 },
        };
    }

} // namespace

TEST(SelectorTest, ParsesTheMarkersAndNames) {
    for (const char* text : { "@s", "@p", "@r", "@a", "@e" }) {
        const auto selector = parse(text);
        ASSERT_TRUE(selector.has_value()) << text;
        EXPECT_EQ(selector->marker, text[1]);
        EXPECT_EQ(selector->text, text);
    }

    const auto by_name = parse("Steve");
    ASSERT_TRUE(by_name.has_value());
    EXPECT_EQ(by_name->marker, 0);
    EXPECT_EQ(by_name->name, "steve");   // names match without regard to case
    EXPECT_TRUE(by_name->single);

    EXPECT_FALSE(parse("@z").has_value());
    EXPECT_FALSE(parse("@").has_value());
    EXPECT_FALSE(parse("").has_value());
}

TEST(SelectorTest, ParsesTheArgumentList) {
    const auto selector = parse("@a[limit=2,sort=furthest,distance=..20,name=Alex]");
    ASSERT_TRUE(selector.has_value());
    EXPECT_EQ(selector->limit, 2);
    EXPECT_EQ(selector->sort, Sort::Furthest);
    EXPECT_TRUE(selector->has_distance);
    EXPECT_DOUBLE_EQ(selector->distance_min, 0.0);
    EXPECT_DOUBLE_EQ(selector->distance_max, 20.0);
    EXPECT_EQ(selector->name_filter, "Alex");

    const auto bounded = parse("@e[distance=4..12]");
    ASSERT_TRUE(bounded.has_value());
    EXPECT_DOUBLE_EQ(bounded->distance_min, 4.0);
    EXPECT_DOUBLE_EQ(bounded->distance_max, 12.0);
}

TEST(SelectorTest, RejectsArgumentsItCannotHonour) {
    EXPECT_FALSE(parse("@a[gamemode=creative]").has_value());
    EXPECT_FALSE(parse("@a[limit=x]").has_value());
    EXPECT_FALSE(parse("@a[distance=far]").has_value());
    EXPECT_FALSE(parse("@a[limit=1").has_value());
    EXPECT_FALSE(parse("@a[limit]").has_value());
}

TEST(SelectorTest, SelfIsTheCaller) {
    const auto selector = parse("@s");
    ASSERT_TRUE(selector.has_value());
    const auto targets = players();
    const auto matched = resolve(*selector, targets[0], targets);
    ASSERT_EQ(matched.size(), 1u);
    EXPECT_EQ(matched.front().entity, 1);
}

TEST(SelectorTest, PicksTheNearestPlayer) {
    const auto selector = parse("@p");
    ASSERT_TRUE(selector.has_value());
    const auto targets = players();
    const auto matched = resolve(*selector, targets[0], targets);
    ASSERT_EQ(matched.size(), 1u);
    EXPECT_EQ(matched.front().entity, 1);

    // From a point next to Steve, @p is Steve.
    const Target source{ 1, "Alex", 9.0, 64.0, 0.0 };
    const auto from_there = resolve(*selector, source, targets);
    ASSERT_EQ(from_there.size(), 1u);
    EXPECT_EQ(from_there.front().entity, 2);
}

TEST(SelectorTest, RandomPicksExactlyOne) {
    const auto selector = parse("@r");
    ASSERT_TRUE(selector.has_value());
    const auto targets = players();
    for (int i = 0; i < 20; ++i) {
        const auto matched = resolve(*selector, targets[0], targets);
        ASSERT_EQ(matched.size(), 1u);
        EXPECT_GE(matched.front().entity, 1);
        EXPECT_LE(matched.front().entity, 3);
    }
}

TEST(SelectorTest, AIsEveryoneAndLimitCutsTheList) {
    const auto selector = parse("@a");
    ASSERT_TRUE(selector.has_value());
    const auto targets = players();
    EXPECT_EQ(resolve(*selector, targets[0], targets).size(), 3u);

    const auto limited = parse("@a[limit=2,sort=nearest]");
    ASSERT_TRUE(limited.has_value());
    const auto cut = resolve(*limited, targets[0], targets);
    ASSERT_EQ(cut.size(), 2u);
    EXPECT_EQ(cut[0].entity, 1);
    EXPECT_EQ(cut[1].entity, 2);
}

TEST(SelectorTest, NamesMatchWithoutCase) {
    const auto selector = parse("herobrine");
    ASSERT_TRUE(selector.has_value());
    const auto targets = players();
    const auto matched = resolve(*selector, targets[0], targets);
    ASSERT_EQ(matched.size(), 1u);
    EXPECT_EQ(matched.front().entity, 3);

    const auto missing = parse("Notch");
    ASSERT_TRUE(missing.has_value());
    EXPECT_TRUE(resolve(*missing, targets[0], targets).empty());
}

TEST(SelectorTest, DistanceFiltersAroundTheCaller) {
    const auto selector = parse("@a[distance=..10]");
    ASSERT_TRUE(selector.has_value());
    const auto targets = players();
    const auto matched = resolve(*selector, targets[0], targets);
    ASSERT_EQ(matched.size(), 2u);
    EXPECT_EQ(matched[0].entity, 1);
    EXPECT_EQ(matched[1].entity, 2);
}

TEST(SelectorTest, SortFurthestOrdersTheList) {
    const auto selector = parse("@a[sort=furthest]");
    ASSERT_TRUE(selector.has_value());
    const auto targets = players();
    const auto matched = resolve(*selector, targets[0], targets);
    ASSERT_EQ(matched.size(), 3u);
    EXPECT_EQ(matched.front().entity, 3);
    EXPECT_EQ(matched.back().entity, 1);
}
