#include <gtest/gtest.h>

#include "protocol/player_state.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace protocol = terila::protocol;

namespace {

    // A framed packet: varint length, then the id, then the payload.
    std::size_t header_size(const std::vector<std::uint8_t>& frame) {
        std::size_t length = 0;
        std::size_t shift = 0;
        for (std::size_t i = 0; i < frame.size() && i < 5; ++i) {
            length |= static_cast<std::size_t>(frame[i] & 0x7F) << shift;
            shift += 7;
            if ((frame[i] & 0x80) == 0) {
                return i + 1;
            }
        }
        return 0;
    }

} // namespace

TEST(PlayerStateTest, SetHealthCarriesHealthFoodAndSaturation) {
    protocol::packet_ids().play_set_health = 0x6A;

    std::vector<std::uint8_t> frame;
    ASSERT_TRUE(protocol::write_set_health(frame, 14.5f, 18, 3.0f));

    const auto start = header_size(frame);
    ASSERT_GT(start, 0u);
    ASSERT_EQ(frame[start], 0x6A);

    // float health, varint food, float saturation.
    std::uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) {
        bits = (bits << 8) | frame[start + 1 + static_cast<std::size_t>(i)];
    }
    float health = 0.0f;
    std::memcpy(&health, &bits, sizeof(health));
    EXPECT_FLOAT_EQ(health, 14.5f);
    EXPECT_EQ(frame[start + 5], 18);
    EXPECT_EQ(frame.size(), start + 1 + 4 + 1 + 4);
}

TEST(PlayerStateTest, SetTimeSendsTheOverworldClock) {
    protocol::packet_ids().play_set_time = 0x73;

    std::vector<std::uint8_t> frame;
    ASSERT_TRUE(protocol::write_set_time(frame, 1200, 6000));

    const auto start = header_size(frame);
    ASSERT_GT(start, 0u);
    EXPECT_EQ(frame[start], 0x73);

    // game time (8 bytes, big endian), one clock, clock id 0, var-long ticks, two floats.
    std::int64_t game_time = 0;
    for (int i = 0; i < 8; ++i) {
        game_time = (game_time << 8) | frame[start + 1 + static_cast<std::size_t>(i)];
    }
    EXPECT_EQ(game_time, 1200);
    EXPECT_EQ(frame[start + 9], 1) << "one clock in the map";
    EXPECT_EQ(frame[start + 10], 0) << "minecraft:overworld is clock 0";

    // 6000 fits in a two byte var-long: (6000 & 0x7F) | 0x80 = 0xF0, then 6000 >> 7 = 0x2E.
    EXPECT_EQ(frame[start + 11], 0xF0);
    EXPECT_EQ(frame[start + 12], 0x2E);
    EXPECT_EQ(frame.size(), start + 13 + 8);
}

TEST(PlayerStateTest, RespawnStartsWithTheOverworldSpawnInfo) {
    protocol::packet_ids().play_respawn = 0x54;

    std::vector<std::uint8_t> frame;
    ASSERT_TRUE(protocol::write_respawn(frame, "minecraft:overworld", 42, 0, 63));

    const auto start = header_size(frame);
    ASSERT_GT(start, 0u);
    EXPECT_EQ(frame[start], 0x54);
    EXPECT_EQ(frame[start + 1], 0) << "dimension type 0 (overworld)";

    const std::string name(reinterpret_cast<const char*>(&frame[start + 3]),
        frame[start + 2]);
    EXPECT_EQ(name, "minecraft:overworld");
    // The keep-data byte sits at the very end.
    EXPECT_EQ(frame.back(), 0);
}

TEST(PlayerStateTest, RespawnAndCombatKillUseTheirIds) {
    protocol::packet_ids().play_player_combat_kill = 0x4C;
    std::vector<std::uint8_t> frame;
    ASSERT_TRUE(protocol::write_player_combat_kill(frame, 1000, "Player was slain by Zombie"));
    const auto start = header_size(frame);
    ASSERT_GT(start, 0u);
    EXPECT_EQ(frame[start], 0x4C);
    EXPECT_GT(frame.size(), start + 3u) << "the id and the message are both there";
}

TEST(PlayerStateTest, ClientCommandAndInteractAreRead) {
    // client_command: a single varint action.
    const std::vector<std::uint8_t> command = { 0x00 };
    const auto action = protocol::read_client_command(command);
    ASSERT_TRUE(action.has_value());
    EXPECT_EQ(*action, 0) << "0 is the respawn button";

    // interact: entity id, hand, three floats of click position, secondary flag.
    std::vector<std::uint8_t> interact = { 0x0A, 0x00 };
    interact.insert(interact.end(), 12, 0);
    interact.push_back(0x01);

    const auto parsed = protocol::read_interact(interact);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->entity_id, 10);
    EXPECT_EQ(parsed->hand, 0);
    EXPECT_TRUE(parsed->secondary);
}
