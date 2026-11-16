#include <gtest/gtest.h>

#include "network/player_hub.hpp"

#include <cstdint>
#include <vector>

namespace {

    struct Delivered {
        int count = 0;
        std::vector<std::int32_t> who;
    };

    void join(terila::network::PlayerHub& hub, std::int32_t entity_id, double x, double z,
        Delivered& delivered) {
        hub.join(entity_id, {}, "player" + std::to_string(entity_id), 0, {},
            terila::network::PlayerHub::Position{ x, 0.0, z, 0.0f, 0.0f },
            [](const std::vector<std::uint8_t>&) {});
        hub.set_giver(entity_id,
            [&delivered, entity_id](std::int32_t, std::int32_t,
                const terila::item::ComponentsPtr&) {
                ++delivered.count;
                delivered.who.push_back(entity_id);
                return true;
            });
    }

} // namespace

TEST(PlayerHubTest, GiveToAllReachesEveryMember) {
    terila::network::PlayerHub hub;
    Delivered first;
    Delivered second;
    join(hub, 11, 0.0, 0.0, first);
    join(hub, 22, 40.0, 0.0, second);

    EXPECT_EQ(hub.give_to_all(1050, 1, nullptr), 2u);
    EXPECT_EQ(first.count, 1);
    EXPECT_EQ(second.count, 1);
}

TEST(PlayerHubTest, GiveToOneLeavesTheOthersAlone) {
    terila::network::PlayerHub hub;
    Delivered first;
    Delivered second;
    join(hub, 11, 0.0, 0.0, first);
    join(hub, 22, 40.0, 0.0, second);

    EXPECT_TRUE(hub.give_to(22, 1050, 1, nullptr));
    EXPECT_EQ(first.count, 0);
    EXPECT_EQ(second.count, 1);
    EXPECT_FALSE(hub.give_to(99, 1050, 1, nullptr));
}

TEST(PlayerHubTest, NearestIsTheClosestColumn) {
    terila::network::PlayerHub hub;
    Delivered close_by;
    Delivered far_away;
    join(hub, 11, 3.0, 4.0, close_by);
    join(hub, 22, 80.0, 0.0, far_away);

    EXPECT_EQ(hub.nearest_to(0.0, 0.0), 11);
    EXPECT_EQ(hub.nearest_to(70.0, 0.0), 22);
    EXPECT_EQ(hub.nearest_to(-5.0, -5.0), 11);
}

TEST(PlayerHubTest, LeavingRemovesTheGiver) {
    terila::network::PlayerHub hub;
    Delivered first;
    join(hub, 11, 0.0, 0.0, first);
    hub.leave(11);

    EXPECT_EQ(hub.give_to_all(1050, 1, nullptr), 0u);
    EXPECT_EQ(first.count, 0);
    EXPECT_EQ(hub.random_member(), -1);
}
