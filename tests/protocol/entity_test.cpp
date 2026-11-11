#include <gtest/gtest.h>

#include "protocol/entity.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <vector>

namespace {

    // A vanilla 26.3 Add Entity capture: zombie (type 154) with movement and rotation.
    const std::uint8_t kVanillaUuid[16] = {
        0xC9, 0x2E, 0x07, 0x37, 0xE3, 0xC4, 0x40, 0xD3,
        0x97, 0x33, 0xF7, 0xD1, 0x44, 0x4E, 0x20, 0xFE
    };

    std::vector<std::uint8_t> vanilla_add_entity_frame() {
        return {
            0x36, 0x01, 0x36,                                     // length, packet id, entity id
            0xC9, 0x2E, 0x07, 0x37, 0xE3, 0xC4, 0x40, 0xD3,
            0x97, 0x33, 0xF7, 0xD1, 0x44, 0x4E, 0x20, 0xFE,       // uuid
            0x9A, 0x01,                                           // type 154 (zombie)
            0x40, 0x2F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       // x 15.5
            0xC0, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       // y -28.0
            0xC0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       // z -2.5
            0xF9, 0xFF, 0x7F, 0xFE, 0xEB, 0xED,                   // movement
            0x00, 0x76, 0x76,                                     // pitch, yaw, head yaw
            0x00                                                  // data
        };
    }

}

TEST(EntityTest, AddEntityMatchesVanillaCapture) {
    terila::protocol::packet_ids().play_add_entity = 0x01;

    std::array<std::uint8_t, 16> uuid{};
    std::copy(std::begin(kVanillaUuid), std::end(kVanillaUuid), uuid.begin());

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(terila::protocol::write_add_entity(out, 54, uuid, 154,
        15.5, -28.0, -2.5,
        165.9375f, 0.0f, 165.9375f,
        -1537, 32766, -5139));

    EXPECT_EQ(out, vanilla_add_entity_frame());
}

// A vanilla capture of a stationary cow: the movement field collapses to one 0x00 byte.
TEST(EntityTest, AddEntitySendsZeroMovementAsSingleByte) {
    terila::protocol::packet_ids().play_add_entity = 0x01;

    std::array<std::uint8_t, 16> uuid{};
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(terila::protocol::write_add_entity(out, 1, uuid, 30,
        28.0, 63.0, 61.0, 0.0f, 0.0f, 0.0f));

    const std::vector<std::uint8_t> expected = {
        0x30, 0x01, 0x01,                                     // length, packet id, entity id
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       // uuid
        0x1E,                                                 // type 30 (cow)
        0x40, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       // x 28.0
        0x40, 0x4F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,       // y 63.0
        0x40, 0x4E, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,       // z 61.0
        0x00,                                                 // zero movement
        0x00, 0x00, 0x00,                                     // pitch, yaw, head yaw
        0x00                                                  // data
    };

    EXPECT_EQ(out, expected);
}

TEST(EntityTest, AddEntityIsFiftyFiveBytesForHighEntityId) {
    terila::protocol::packet_ids().play_add_entity = 0x01;

    std::array<std::uint8_t, 16> uuid{};
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(terila::protocol::write_add_entity(out, 1000, uuid, 154,
        1.0, 2.0, 3.0, 0.0f, 0.0f, 0.0f));

    // 1 length + 1 id + 2 entity id + 16 uuid + 2 type + 24 position
    // + 1 zero movement + 3 angles + 1 data
    EXPECT_EQ(out.size(), 51u);
}

// Vanilla capture: 0x36 0x31 0x01 0x00 0x00 0x00 0x00 0x00 0x00
// properties 0x01 is onGround with step count 0, selecting the linear variant.
TEST(EntityTest, MoveEntityPosMatchesVanillaCapture) {
    terila::protocol::packet_ids().play_move_entity_pos = 0x36;

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(terila::protocol::write_move_entity_pos(out, 0x31, 0, 0, 0, true));

    const std::vector<std::uint8_t> expected = {
        0x09, 0x36, 0x31,       // length, packet id, entity id
        0x01,                   // properties: onGround, zero steps
        0x00, 0x00,             // dx
        0x00, 0x00,             // dy
        0x00, 0x00              // dz
    };

    EXPECT_EQ(out, expected);
}

// Linear VecDelta keeps the rotation bytes directly after the delta, no trailer.
TEST(EntityTest, MoveEntityPosRotUsesLinearVecDelta) {
    terila::protocol::packet_ids().play_move_entity_pos_rot = 0x37;

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(terila::protocol::write_move_entity_pos_rot(out, 26,
        0x0304, static_cast<std::int16_t>(0xC6FA), 0x42FD, 123.75f, -106.875f, true));

    const std::vector<std::uint8_t> expected = {
        0x0B, 0x37, 0x1A,       // length, packet id, entity id
        0x01,                   // properties: onGround, zero steps
        0x03, 0x04,             // dx
        0xC6, 0xFA,             // dy
        0x42, 0xFD,             // dz
        0x58,                   // yaw
        0xB4                    // pitch
    };

    EXPECT_EQ(out, expected);
}
