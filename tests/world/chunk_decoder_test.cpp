#include <gtest/gtest.h>
#include "world/chunk.hpp"
#include "world/chunk_encoder.hpp"
#include "world/chunk_decoder.hpp"
#include "world/flat_generator.hpp"

#include <optional>
#include <string_view>

using namespace terila::world;

// Deliberately non-sequential IDs to catch accidental hardcoding in the generator.
static std::optional<std::int32_t> mock_block(std::string_view name) {
    if (name == "minecraft:air")         return 0;
    if (name == "minecraft:bedrock")     return 7;
    if (name == "minecraft:dirt")        return 13;
    if (name == "minecraft:grass_block") return 42;
    return std::nullopt;
}

static std::optional<std::int32_t> mock_biome(std::string_view name) {
    if (name == "minecraft:plains") return 999;
    return std::nullopt;
}

static Chunk build_flat(std::int32_t cx, std::int32_t cz) {
    flat::FlatSpec spec;
    auto opt = flat::generate(cx, cz, spec, mock_block, mock_biome);
    if (!opt) throw std::runtime_error("flat::generate failed in test");
    return *opt;
}

TEST(ChunkDecoderTest, FlatChunkRoundTrip) {
    auto chunk = build_flat(0, 0);
    auto wire1 = encode::encode(chunk);

    auto decoded = decode::decode(wire1.data(), wire1.size(), 24);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->x, 0);
    EXPECT_EQ(decoded->z, 0);
    EXPECT_EQ(decoded->sections.size(), chunk.sections.size());
    EXPECT_EQ(decoded->heightmaps.size(), chunk.heightmaps.size());

    auto wire2 = encode::encode(*decoded);
    EXPECT_EQ(wire1, wire2);
}

TEST(ChunkDecoderTest, FlatChunkBottomSection) {
    auto chunk = build_flat(5, 7);
    auto wire = encode::encode(chunk);
    auto decoded = decode::decode(wire.data(), wire.size(), 24);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->x, 5);
    EXPECT_EQ(decoded->z, 7);
    ASSERT_FALSE(decoded->sections.empty());

    const auto& bottom = decoded->sections[0];
    // palette indices: 0=air, 1=bedrock, 2=dirt, 3=grass (matches mock)
    EXPECT_EQ(bottom.block_states[0 * 256 + 0 * 16 + 0], 1);  // y=0 bedrock
    EXPECT_EQ(bottom.block_states[1 * 256 + 0 * 16 + 0], 2);  // y=1 dirt
    EXPECT_EQ(bottom.block_states[3 * 256 + 0 * 16 + 0], 2);  // y=3 dirt
    EXPECT_EQ(bottom.block_states[4 * 256 + 0 * 16 + 0], 3);  // y=4 grass
    EXPECT_EQ(bottom.block_states[5 * 256 + 0 * 16 + 0], 0);  // y=5 air

    // palette preserves real IDs (7=bedrock, 13=dirt, 42=grass)
    ASSERT_EQ(bottom.block_palette.size(), 4u);
    EXPECT_EQ(bottom.block_palette[0], 0);   // air
    EXPECT_EQ(bottom.block_palette[1], 7);   // bedrock
    EXPECT_EQ(bottom.block_palette[2], 13);  // dirt
    EXPECT_EQ(bottom.block_palette[3], 42);  // grass
}

TEST(ChunkDecoderTest, BiomeIdFlowsThrough) {
    auto chunk = build_flat(0, 0);
    auto wire = encode::encode(chunk);
    auto decoded = decode::decode(wire.data(), wire.size(), 24);

    ASSERT_TRUE(decoded.has_value());
    for (const auto& sec : decoded->sections) {
        ASSERT_EQ(sec.biome_palette.size(), 1u);
        EXPECT_EQ(sec.biome_palette[0], 999);  // mock plains id
    }
}

TEST(ChunkDecoderTest, HeightmapsPreserved) {
    auto chunk = build_flat(0, 0);
    auto wire = encode::encode(chunk);
    auto decoded = decode::decode(wire.data(), wire.size(), 24);

    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->heightmaps.size(), 3u);
    EXPECT_EQ(decoded->heightmaps[0].type_id, 1);
    // 256 columns x 9 bits = 2304 bits = 36 longs.
    EXPECT_EQ(decoded->heightmaps[0].longs.size(), 36u);
    // Every column stores the first air block above the grass, relative to min_y.
    const std::uint64_t first_column =
        static_cast<std::uint64_t>(decoded->heightmaps[0].longs[0]) & 0x1FFu;
    EXPECT_EQ(first_column, 5u);
    EXPECT_EQ(decoded->heightmaps[1].type_id, 4);
    EXPECT_EQ(decoded->heightmaps[2].type_id, 5);
}

TEST(ChunkDecoderTest, TruncatedDataDoesNotCrash) {
    auto chunk = build_flat(0, 0);
    auto wire = encode::encode(chunk);
    auto decoded = decode::decode(wire.data(), wire.size() / 2, 24);
    (void)decoded;  // may fail — that's fine, just must not crash
}