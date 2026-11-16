#include <gtest/gtest.h>

#include "protocol/block.hpp"
#include "protocol/play.hpp"

#include <vector>

namespace {

    void put_int64(std::vector<std::uint8_t>& out, std::uint64_t value) {
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
        }
    }

    void put_varint(std::vector<std::uint8_t>& out, std::uint32_t value) {
        terila::protocol::write_varint(out, static_cast<std::int32_t>(value));
    }

    void put_short(std::vector<std::uint8_t>& out, std::int32_t value) {
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }

} // namespace

// The shovel bug: a pick from the creative menu has to arrive as the item it is. Reading
// the stack one byte late turned every pick into an empty slot, so the server thought the
// hand was empty while the client showed the tool.
TEST(ReadCreativeSlot, ReadsTheStackRightAfterTheSlotNumber) {
    std::vector<std::uint8_t> payload;
    put_short(payload, 36);                 // hotbar slot 0
    put_varint(payload, 1);                 // one shovel
    put_varint(payload, 1052);              // minecraft:diamond_pickaxe
    std::vector<std::uint8_t> components{ 0x00 };   // empty component patch
    payload.insert(payload.end(), components.begin(), components.end());

    const auto update = terila::protocol::read_creative_slot(payload.data(), payload.size());

    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->slot, 36);
    EXPECT_EQ(update->count, 1);
    EXPECT_EQ(update->item_id, 1052);
}

TEST(ReadCreativeSlot, AnEmptySlotCarriesNoItem) {
    std::vector<std::uint8_t> payload;
    put_short(payload, 44);
    put_varint(payload, 0);

    const auto update = terila::protocol::read_creative_slot(payload.data(), payload.size());

    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->slot, 44);
    EXPECT_EQ(update->count, 0);
    EXPECT_EQ(update->item_id, 0);
}

TEST(ReadCreativeSlot, RejectsATruncatedPacket) {
    std::vector<std::uint8_t> payload;
    put_short(payload, 36);

    EXPECT_FALSE(terila::protocol::read_creative_slot(payload.data(), payload.size()).has_value());
}

// use_item_on: hand, then the hit result (position, face, cursor, inside-block flag), then
// the sequence. The face and the position drive the placement, so they are what has to
// survive the cursor fields.
TEST(ReadUseItemOn, ReadsPositionFaceAndSequence) {
    std::vector<std::uint8_t> payload;
    put_varint(payload, 0);                                     // main hand
    put_int64(payload, terila::protocol::pack_block_pos(264, 65, 264));
    put_varint(payload, 1);                                     // top face
    for (int i = 0; i < 3; ++i) {
        payload.insert(payload.end(), { 0x00, 0x00, 0x00, 0x00 });   // cursor at the corner
    }
    payload.push_back(0x01);                                    // inside the block
    put_varint(payload, 42);                                    // sequence

    const auto use = terila::protocol::read_use_item_on(payload.data(), payload.size());

    ASSERT_TRUE(use.has_value());
    EXPECT_EQ(use->pos.x, 264);
    EXPECT_EQ(use->pos.y, 65);
    EXPECT_EQ(use->pos.z, 264);
    EXPECT_EQ(use->face, 1);
    EXPECT_EQ(use->sequence, 42);
}

TEST(ReadUseItemOn, AMissingSequenceStillLeavesAPlacement) {
    std::vector<std::uint8_t> payload;
    put_varint(payload, 0);
    put_int64(payload, terila::protocol::pack_block_pos(-17, 70, 3));
    put_varint(payload, 4);
    payload.insert(payload.end(), 13, 0x00);   // cursor and inside flag, no sequence

    const auto use = terila::protocol::read_use_item_on(payload.data(), payload.size());

    ASSERT_TRUE(use.has_value());
    EXPECT_EQ(use->pos.x, -17);
    EXPECT_EQ(use->pos.z, 3);
    EXPECT_EQ(use->face, 4);
    EXPECT_EQ(use->sequence, 0);
}
