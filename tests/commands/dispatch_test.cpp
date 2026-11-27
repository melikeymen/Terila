#include <gtest/gtest.h>

#include "commands/registry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace commands = terila::commands;

namespace {

    struct FakeGame final : commands::GameApi {
        std::vector<commands::Target> members;
        std::vector<commands::Target> mobs;
        std::vector<std::string> replies;
        std::vector<std::int32_t> kills;
        std::vector<std::string> broadcasts;
        std::int64_t world_seed = 12345;
        int player_limit = 20;
        std::vector<std::pair<std::int32_t, std::int32_t>> gifts;
        std::vector<std::string> teleports;
        std::vector<std::pair<std::int32_t, int>> modes;
        std::vector<std::string> summons;
        std::vector<std::string> messages;
        std::int64_t clock = 0;
        std::int64_t day = 0;
        bool is_night = false;
        int syncs = 0;

        FakeGame() {
            members = {
                commands::Target{ 1, "Alex", 0.0, 64.0, 0.0, 90.0f, 0.0f },
                commands::Target{ 2, "Steve", 10.0, 64.0, 0.0, 180.0f, 0.0f },
            };
        }

        std::vector<commands::Target> players() const override { return members; }

        bool give(std::int32_t entity, std::int32_t item_id, std::int32_t count,
            const terila::item::ComponentsPtr&) override {
            gifts.emplace_back(entity, item_id * 1000 + count);
            return true;
        }

        bool teleport(std::int32_t entity, double x, double y, double z, float yaw,
            float pitch) override {
            teleports.push_back(std::to_string(entity) + ":" + std::to_string(x) + "," +
                std::to_string(y) + "," + std::to_string(z) + ":" + std::to_string(yaw) +
                "," + std::to_string(pitch));
            return true;
        }

        bool set_gamemode(std::int32_t entity, int mode) override {
            modes.emplace_back(entity, mode);
            return true;
        }

        int gamemode(std::int32_t) const override { return 0; }

        bool summon(std::string_view type, double x, double y, double z) override {
            summons.push_back(std::string(type) + "@" + std::to_string(x) + "," +
                std::to_string(y) + "," + std::to_string(z));
            return true;
        }

        bool summonable(std::string_view type) const override {
            return type == "minecraft:pig";
        }

        bool tell(std::int32_t entity, std::string_view text) override {
            messages.push_back(std::to_string(entity) + ":" + std::string(text));
            return true;
        }

        std::vector<commands::Target> entities() const override {
            std::vector<commands::Target> out = members;
            out.insert(out.end(), mobs.begin(), mobs.end());
            return out;
        }

        bool kill(std::int32_t entity, std::string_view) override {
            kills.push_back(entity);
            return true;
        }

        std::size_t broadcast(std::string_view text) override {
            broadcasts.emplace_back(text);
            return members.size();
        }

        std::int64_t seed() const override { return world_seed; }
        int max_players() const override { return player_limit; }

        std::int64_t game_time() const override { return clock; }
        std::int64_t time_of_day() const override { return day; }
        bool night() const override { return is_night; }
        void set_time(std::int64_t ticks) override { day = ticks; }
        void add_time(std::int64_t ticks) override { day += ticks; }
        void sync_time() override { ++syncs; }
    };

    struct Harness {
        terila::data::RegistryData data;
        terila::item::ComponentIds components;
        commands::Registry registry;
        FakeGame game;
        std::vector<std::string> replies;

        Harness() : registry(data, components) {
            data.command_argument_types = {
                { "brigadier:integer", 3 },
                { "brigadier:string", 5 },
                { "minecraft:entity", 6 },
                { "minecraft:item_stack", 14 },
                { "minecraft:vec3", 15 },
                { "minecraft:rotation", 16 },
                { "minecraft:gamemode", 17 },
                { "minecraft:message", 20 },
                { "minecraft:resource_location", 21 },
            };
            data.item_ids = { { "minecraft:stone", 1 }, { "minecraft:diamond", 2 } };
            registry.install_all();
        }

        commands::Origin origin() {
            commands::Origin origin;
            origin.entity = 1;
            origin.name = "Alex";
            origin.x = 0.0;
            origin.y = 64.0;
            origin.z = 0.0;
            origin.yaw = 90.0f;
            origin.capability = commands::Capability::Owner;
            origin.reply = [this](std::string_view text) {
                replies.emplace_back(text);
            };
            origin.game = &game;
            game.replies = replies;
            return origin;
        }
    };

} // namespace

TEST(CommandDispatchTest, GivesAnItemToTheCaller) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result = harness.registry.run(origin, "give minecraft:stone 3", scope);

    EXPECT_TRUE(result.ok);
    ASSERT_EQ(harness.game.gifts.size(), 1u);
    EXPECT_EQ(harness.game.gifts[0].first, 1);
    EXPECT_EQ(harness.game.gifts[0].second, 1003);   // item 1, count 3
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("Gave minecraft:stone x3"),
        std::string::npos);
}

TEST(CommandDispatchTest, GivesToASelector) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result = harness.registry.run(origin, "give @a minecraft:diamond", scope);

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(harness.game.gifts.size(), 2u);
    EXPECT_EQ(result.count, 2);
}

TEST(CommandDispatchTest, SetTimeMovesTheClock) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result = harness.registry.run(origin, "time set day", scope);

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(harness.game.day, 1000);
    EXPECT_EQ(harness.game.syncs, 1);
}

TEST(CommandDispatchTest, GamemodeChangesTheCaller) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result = harness.registry.run(origin, "gamemode creative", scope);

    EXPECT_TRUE(result.ok);
    ASSERT_EQ(harness.game.modes.size(), 1u);
    EXPECT_EQ(harness.game.modes[0].first, 1);
    EXPECT_EQ(harness.game.modes[0].second, 1);
}

TEST(CommandDispatchTest, TeleportResolvesAbsoluteAndRelativeCoordinates) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    ASSERT_TRUE(harness.registry.run(origin, "tp 1 2 3", scope).ok);
    ASSERT_EQ(harness.game.teleports.size(), 1u);
    EXPECT_EQ(harness.game.teleports[0], "1:1.000000,2.000000,3.000000:90.000000,0.000000");

    ASSERT_TRUE(harness.registry.run(origin, "tp ~ ~1 ~", scope).ok);
    ASSERT_EQ(harness.game.teleports.size(), 2u);
    // Relative to the victim, which is the caller standing at y=64.
    EXPECT_EQ(harness.game.teleports[1],
        "1:0.000000,65.000000,0.000000:90.000000,0.000000");
}

TEST(CommandDispatchTest, TeleportToAPlayerUsesItsFacing) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    ASSERT_TRUE(harness.registry.run(origin, "tp Steve", scope).ok);
    ASSERT_EQ(harness.game.teleports.size(), 1u);
    EXPECT_EQ(harness.game.teleports[0],
        "1:10.000000,64.000000,0.000000:180.000000,0.000000");
}

TEST(CommandDispatchTest, SummonRefusesAnUnknownEntity) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_FALSE(harness.registry.run(origin, "summon minecraft:dragon 1 2 3", scope).ok);

    const auto result = harness.registry.run(origin, "summon pig 1 2 3", scope);
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(harness.game.summons.size(), 1u);
    EXPECT_EQ(harness.game.summons[0], "minecraft:pig@1.000000,2.000000,3.000000");
}

TEST(CommandDispatchTest, MessageReachesEachTarget) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result = harness.registry.run(origin, "msg Steve hello there", scope);

    EXPECT_TRUE(result.ok);
    ASSERT_EQ(harness.game.messages.size(), 1u);
    EXPECT_EQ(harness.game.messages[0], "2:Alex whispers to you: hello there");
}

TEST(CommandDispatchTest, ExecuteRunsTheTailAsAnotherPlayer) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result =
        harness.registry.run(origin, "execute as Steve run give minecraft:stone", scope);

    EXPECT_TRUE(result.ok);
    ASSERT_EQ(harness.game.gifts.size(), 1u);
    EXPECT_EQ(harness.game.gifts[0].first, 2);
}

TEST(CommandDispatchTest, UnknownCommandIsReported) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result = harness.registry.run(origin, "nope", scope);

    EXPECT_FALSE(result.ok);
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("unknown"), std::string::npos);
}

TEST(CommandDispatchTest, IncompleteCommandIsReported) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_FALSE(harness.registry.run(origin, "time set", scope).ok);
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("incomplete"), std::string::npos);
}

TEST(CommandDispatchTest, KillWithoutATargetKillsTheCaller) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result = harness.registry.run(origin, "kill", scope);

    EXPECT_TRUE(result.ok);
    ASSERT_EQ(harness.game.kills.size(), 1u);
    EXPECT_EQ(harness.game.kills[0], 1);
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("Killed Alex"), std::string::npos);
}

TEST(CommandDispatchTest, KillWithASelectorKillsEveryMatch) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result = harness.registry.run(origin, "kill @a", scope);

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.count, 2);
    EXPECT_EQ(harness.game.kills, (std::vector<std::int32_t>{ 1, 2 }));
}

TEST(CommandDispatchTest, KillReachesAMobThroughTheWideSelector) {
    Harness harness;
    harness.game.mobs.push_back(
        commands::Target{ 77, "minecraft:zombie", 3.0, 64.0, 0.0, 0.0f, 0.0f });
    auto origin = harness.origin();
    commands::RunScope scope;

    const auto result = harness.registry.run(origin, "kill @e[limit=3]", scope);

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.count, 3);
    ASSERT_EQ(harness.game.kills.size(), 3u);
    EXPECT_EQ(harness.game.kills[2], 77);
}

TEST(CommandDispatchTest, SeedReportsTheWorldSeed) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_TRUE(harness.registry.run(origin, "seed", scope).ok);
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("12345"), std::string::npos);
}

TEST(CommandDispatchTest, ListReportsTheOnlinePlayers) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_TRUE(harness.registry.run(origin, "list", scope).ok);
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("2 of a max of 20"), std::string::npos);
    EXPECT_NE(harness.replies.front().find("Alex"), std::string::npos);
}

TEST(CommandDispatchTest, SayReachesEveryPlayer) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_TRUE(harness.registry.run(origin, "say hello there", scope).ok);
    ASSERT_EQ(harness.game.broadcasts.size(), 1u);
    EXPECT_EQ(harness.game.broadcasts[0], "[Alex] hello there");
}

TEST(CommandDispatchTest, MeAnnouncesAnAction) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_TRUE(harness.registry.run(origin, "me waves", scope).ok);
    ASSERT_EQ(harness.game.broadcasts.size(), 1u);
    EXPECT_EQ(harness.game.broadcasts[0], "* Alex waves");
}

TEST(CommandDispatchTest, HelpWithoutAnArgumentListsTheCommands) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_TRUE(harness.registry.run(origin, "help", scope).ok);
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("give"), std::string::npos);
    EXPECT_NE(harness.replies.front().find("kill"), std::string::npos);
}

TEST(CommandDispatchTest, HelpWithACommandShowsItsForms) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_TRUE(harness.registry.run(origin, "help give", scope).ok);
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("/give <item>"), std::string::npos);
}

TEST(CommandDispatchTest, HelpRefusesAnUnknownCommand) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_FALSE(harness.registry.run(origin, "help nope", scope).ok);
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("No such command"), std::string::npos);
}

TEST(CommandDispatchTest, VersionNamesTheServer) {
    Harness harness;
    auto origin = harness.origin();
    commands::RunScope scope;

    EXPECT_TRUE(harness.registry.run(origin, "version", scope).ok);
    ASSERT_FALSE(harness.replies.empty());
    EXPECT_NE(harness.replies.front().find("Terila"), std::string::npos);
}
