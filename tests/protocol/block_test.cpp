#include <gtest/gtest.h>

#include "protocol/block.hpp"
#include "world/block_world.hpp"

#include <tuple>
#include <vector>

TEST(BlockPosTest, RoundTripsNegativeAndPositiveCoords) {
    const std::tuple<std::int32_t, std::int32_t, std::int32_t> cases[] = {
        { 8, -59, 8 },
        { -33, 4, -17 },
        { 0, 0, 0 },
        { -33554432, -2048, 33554431 },
    };

    for (const auto& [x, y, z] : cases) {
        const auto packed = terila::protocol::pack_block_pos(x, y, z);

        std::vector<std::uint8_t> bytes(8);
        for (int i = 0; i < 8; ++i) {
            bytes[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((static_cast<std::uint64_t>(packed) >> ((7 - i) * 8)) & 0xFF);
        }

        const auto out = terila::protocol::unpack_block_pos(bytes.data(), bytes.size());
        EXPECT_EQ(out.x, x);
        EXPECT_EQ(out.y, y);
        EXPECT_EQ(out.z, z);
    }
}

TEST(BlockUpdateTest, WritesPackedPositionThenStateVarint) {
    terila::protocol::packet_ids().play_block_update = 0x08;

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(terila::protocol::write_block_update(out, 1, 2, 3, 5));

    ASSERT_EQ(out.size(), 11u);
    EXPECT_EQ(out[0], 0x0A);   // packet length: id + 8 byte position + 1 byte state
    EXPECT_EQ(out[1], 0x08);
    EXPECT_EQ(out[10], 0x05);
}

TEST(BlockWorldTest, StoresOverridesPerChunk) {
    terila::world::BlockWorld world;
    const terila::world::BlockPos pos{ 5, -60, 5 };

    EXPECT_FALSE(world.chunk_modified(0, 0));
    EXPECT_EQ(world.get(pos, 7), 7);

    world.set(pos, 42);
    EXPECT_TRUE(world.chunk_modified(0, 0));
    EXPECT_EQ(world.get(pos, 7), 42);
    EXPECT_EQ(world.get(terila::world::BlockPos{ 6, -60, 5 }, 7), 7);
}

TEST(BlockWorldTest, ApplyGrowsThePaletteAndTracksBlockCount) {
    terila::world::Chunk chunk;
    chunk.min_y = -64;
    chunk.section_count = 24;
    chunk.sections.resize(24);

    terila::world::ChunkSection& bottom = chunk.sections[0];
    bottom.block_bits = 0;
    bottom.block_palette = { 0 };      // air
    bottom.block_states.assign(4096, 0);
    bottom.block_count = 0;

    terila::world::BlockWorld world;
    world.set(terila::world::BlockPos{ 1, -64, 1 }, 99);
    world.apply(chunk, 0);

    const int index = (0 * 16 + 1) * 16 + 1;
    EXPECT_EQ(terila::world::section_state(chunk.sections[0], index), 99);
    EXPECT_EQ(chunk.sections[0].block_count, 1);
    EXPECT_EQ(chunk.sections[0].block_bits, 4);
}
