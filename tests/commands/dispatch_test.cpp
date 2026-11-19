#include <gtest/gtest.h>

#include "commands/dispatch.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using terila::commands::Context;
using terila::commands::dispatch;
using terila::commands::Target;

namespace {

    struct Fake {
        std::vector<Target> players;
        std::vector<std::string> replies;
        std::vector<std::pair<std::int32_t, std::int32_t>> gifts;
        std::vector<std::string> nested;
        std::int64_t time = 0;
        bool night = false;
        int syncs = 0;
        std::unordered_map<std::string, std::int32_t> item_ids;
        terila::item::ComponentIds component_ids;
        terila::commands::TimeApi time_api;

        Fake() {
            players = {
                Target{ 1, "Alex", 0.0, 64.0, 0.0 },
                Target{ 2, "Steve", 10.0, 64.0, 0.0 },
            };
            item_ids["minecraft:stone"] = 7;
            time_api.game_time = [this] { return time; };
            time_api.time_of_day = [this] { return time; };
            time_api.night = [this] { return night; };
            time_api.set = [this](std::int64_t ticks) { time = ticks; };
            time_api.add = [this](std::int64_t ticks) { time += ticks; };
            time_api.sync = [this] { ++syncs; };
        }

        Context context() {
            Context context;
            context.self = players.front();
            context.players = [this] { return players; };
            context.reply = [this](std::string_view text) { replies.emplace_back(text); };
            context.give = [this](std::int32_t entity, std::int32_t item_id, std::int32_t,
                const terila::item::ComponentsPtr&) {
                gifts.emplace_back(entity, item_id);
                return true;
            };
            context.run = [this](std::string_view line) { nested.emplace_back(line); };
            context.run_as = [this](std::int32_t entity, std::string_view line) {
                nested.push_back(std::to_string(entity) + ":" + std::string(line));
                return true;
            };
            context.item_ids = &item_ids;
            context.component_ids = &component_ids;
            context.time = &time_api;
            return context;
        }
    };

} // namespace

TEST(CommandDispatchTest, GiveWithoutASelectorGoesToTheCaller) {
    Fake fake;
    EXPECT_TRUE(dispatch(fake.context(), "give minecraft:stone 3"));
    ASSERT_EQ(fake.gifts.size(), 1u);
    EXPECT_EQ(fake.gifts.front().first, 1);
    EXPECT_EQ(fake.gifts.front().second, 7);
    ASSERT_EQ(fake.replies.size(), 1u);
    EXPECT_EQ(fake.replies.front(), "Gave minecraft:stone x3");
}

TEST(CommandDispatchTest, GiveWithASelectorReachesEveryMatch) {
    Fake fake;
    EXPECT_TRUE(dispatch(fake.context(), "give @a minecraft:stone"));
    EXPECT_EQ(fake.gifts.size(), 2u);
    ASSERT_EQ(fake.replies.size(), 1u);
    EXPECT_EQ(fake.replies.front(), "Gave minecraft:stone x1 to 2 players");
}

TEST(CommandDispatchTest, GiveTakesAPlayerName) {
    Fake fake;
    EXPECT_TRUE(dispatch(fake.context(), "give Steve minecraft:stone"));
    ASSERT_EQ(fake.gifts.size(), 1u);
    EXPECT_EQ(fake.gifts.front().first, 2);
}

TEST(CommandDispatchTest, GiveRefusesAnUnknownItem) {
    Fake fake;
    EXPECT_TRUE(dispatch(fake.context(), "give minecraft:diamond"));
    EXPECT_TRUE(fake.gifts.empty());
    ASSERT_EQ(fake.replies.size(), 1u);
    EXPECT_EQ(fake.replies.front(), "Unknown item: minecraft:diamond");
}

TEST(CommandDispatchTest, TimeSetMovesTheClockAndSyncs) {
    Fake fake;
    EXPECT_TRUE(dispatch(fake.context(), "time set day"));
    EXPECT_EQ(fake.time, 1000);
    EXPECT_EQ(fake.syncs, 1);
    ASSERT_EQ(fake.replies.size(), 1u);
    EXPECT_EQ(fake.replies.front(), "Set the time to 1000 (day)");
}

TEST(CommandDispatchTest, TimeAddCountsForward) {
    Fake fake;
    fake.time = 500;
    EXPECT_TRUE(dispatch(fake.context(), "time add 250"));
    EXPECT_EQ(fake.time, 750);
    EXPECT_EQ(fake.syncs, 1);
}

TEST(CommandDispatchTest, TimeQueryReportsTheClock) {
    Fake fake;
    fake.time = 13000;
    fake.night = true;
    EXPECT_TRUE(dispatch(fake.context(), "time query daytime"));
    ASSERT_EQ(fake.replies.size(), 1u);
    EXPECT_EQ(fake.replies.front(), "The time is 13000 (night)");

    fake.replies.clear();
    EXPECT_TRUE(dispatch(fake.context(), "time query gametime"));
    ASSERT_EQ(fake.replies.size(), 1u);
    EXPECT_EQ(fake.replies.front(), "The world has run 13000 ticks");
}

TEST(CommandDispatchTest, TimeWithoutAClockIsReported) {
    Fake fake;
    auto context = fake.context();
    context.time = nullptr;
    EXPECT_TRUE(dispatch(context, "time set day"));
    ASSERT_EQ(fake.replies.size(), 1u);
    EXPECT_EQ(fake.replies.front(), "This world has no clock");
}

TEST(CommandDispatchTest, ExecuteAsRunsTheCommandForEveryTarget) {
    Fake fake;
    EXPECT_TRUE(dispatch(fake.context(), "execute as @a run time set night"));
    ASSERT_EQ(fake.nested.size(), 2u);
    EXPECT_EQ(fake.nested[0], "1:time set night");
    EXPECT_EQ(fake.nested[1], "2:time set night");
}

TEST(CommandDispatchTest, ExecuteWithoutTargetsRunsAsTheCaller) {
    Fake fake;
    EXPECT_TRUE(dispatch(fake.context(), "execute run time set day"));
    ASSERT_EQ(fake.nested.size(), 1u);
    EXPECT_EQ(fake.nested.front(), "time set day");
}

TEST(CommandDispatchTest, UnknownCommandIsNotDispatched) {
    Fake fake;
    EXPECT_FALSE(dispatch(fake.context(), "summon minecraft:pig"));
    EXPECT_TRUE(fake.replies.empty());
}
