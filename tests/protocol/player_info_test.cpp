#include <gtest/gtest.h>

#include "protocol/player_info.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace {

    // A frame the client can parse: varint length, varint packet id, then the
    // action bit field, the entry count and the entries in ordinal order.
    std::array<std::uint8_t, 16> sample_uuid() {
        std::array<std::uint8_t, 16> uuid{};
        uuid[0] = 0x11;
        uuid[15] = 0x22;
        return uuid;
    }

}

TEST(PlayerInfoTest, AddEntryMatchesExpectedLayout) {
    terila::protocol::packet_ids().play_player_info_update = 71;

    const auto uuid = sample_uuid();
    const terila::protocol::PlayerListEntry entry{ uuid, "Steve", 1, 0, true };

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(terila::protocol::write_player_info_add(out, { entry }));

    std::vector<std::uint8_t> expected = {
        29,     // frame length: id + 28 payload bytes
        71,     // player_info_update
        0x1D,   // ADD_PLAYER | UPDATE_GAME_MODE | UPDATE_LISTED | UPDATE_LATENCY
        1       // one entry
    };
    expected.insert(expected.end(), uuid.begin(), uuid.end());
    expected.push_back(5);
    expected.insert(expected.end(), { 'S', 't', 'e', 'v', 'e' });
    expected.push_back(0);   // no profile properties
    expected.push_back(1);   // gamemode: creative
    expected.push_back(1);   // listed
    expected.push_back(0);   // latency

    EXPECT_EQ(out, expected);
}

TEST(PlayerInfoTest, AddOfTwoEntriesUsesOneActionHeader) {
    terila::protocol::packet_ids().play_player_info_update = 71;

    const auto uuid = sample_uuid();
    const std::vector<terila::protocol::PlayerListEntry> entries{
        { uuid, "Alex", 0, 0, true },
        { uuid, "Steve", 0, 0, true }
    };

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(terila::protocol::write_player_info_add(out, entries));

    // 1 id + 1 actions + 1 count + 2 * (16 uuid + 1 length + 4 name + 1 props + 1 mode + 1 listed + 1 latency)
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out[0], 54);    // frame length
    EXPECT_EQ(out[1], 71);    // packet id
    EXPECT_EQ(out[2], 0x1D);  // actions once for the whole packet
    EXPECT_EQ(out[3], 2);     // entry count
    EXPECT_EQ(out[4 + 16], 4);   // "Alex"
}

TEST(PlayerInfoTest, RemoveListsRawUuids) {
    terila::protocol::packet_ids().play_player_info_remove = 70;

    const auto uuid = sample_uuid();
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(terila::protocol::write_player_info_remove(out, { uuid }));

    std::vector<std::uint8_t> expected = { 18, 70, 1 };   // length, id, count
    expected.insert(expected.end(), uuid.begin(), uuid.end());

    EXPECT_EQ(out, expected);
}

TEST(PlayerInfoTest, EmptyInputWritesNothing) {
    terila::protocol::packet_ids().play_player_info_update = 71;

    std::vector<std::uint8_t> out;
    EXPECT_TRUE(terila::protocol::write_player_info_add(out, {}));
    EXPECT_TRUE(terila::protocol::write_player_info_remove(out, {}));
    EXPECT_TRUE(out.empty());
}
