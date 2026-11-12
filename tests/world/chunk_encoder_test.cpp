#include <gtest/gtest.h>
#include "world/chunk.hpp"
#include "world/chunk_encoder.hpp"
#include "world/flat_generator.hpp"

#include <optional>
#include <string_view>

using namespace terila::world;

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

TEST(ChunkEncoderTest, EmptyChunkProducesValidBytes) {
    Chunk c;
    c.x = 0; c.z = 0;
    c.section_count = 24;
    c.sections.resize(24);
    for (auto& sec : c.sections) {
        sec.block_count = 0;
        sec.block_bits = 0;
        sec.block_palette = { 0 };
        sec.block_states.assign(4096, 0);
        sec.biome_bits = 0;
        sec.biome_palette = { 0 };
        sec.biome_states.assign(64, 0);
    }
    Heightmap hm;
    hm.type_id = 1;
    hm.longs.assign(36, 0);
    c.heightmaps.push_back(hm);

    auto wire = encode::encode(c);
    EXPECT_GT(wire.size(), 100u);
    EXPECT_EQ(wire[0], 0);
    EXPECT_EQ(wire[1], 0);
    EXPECT_EQ(wire[2], 0);
    EXPECT_EQ(wire[3], 0);
}

TEST(ChunkEncoderTest, FlatChunkNonEmpty) {
    auto chunk = build_flat(0, 0);
    auto wire = encode::encode(chunk);
    EXPECT_GT(wire.size(), 500u);
    EXPECT_EQ(wire[0], 0);
    EXPECT_EQ(wire[3], 0);
}

TEST(ChunkEncoderTest, NegativeCoordsBigEndian) {
    auto chunk = build_flat(-3, -1);
    auto wire = encode::encode(chunk);
    ASSERT_GE(wire.size(), 8u);
    EXPECT_EQ(wire[0], 0xFF);
    EXPECT_EQ(wire[1], 0xFF);
    EXPECT_EQ(wire[2], 0xFF);
    EXPECT_EQ(wire[3], 0xFD);  // -3
    EXPECT_EQ(wire[4], 0xFF);
    EXPECT_EQ(wire[5], 0xFF);
    EXPECT_EQ(wire[6], 0xFF);
    EXPECT_EQ(wire[7], 0xFF);  // -1
}

TEST(FlatGeneratorTest, FailsWhenBedrockUnknown) {
    flat::FlatSpec spec;
    spec.bedrock = "minecraft:not_a_block";
    auto opt = flat::generate(0, 0, spec, mock_block, mock_biome);
    EXPECT_FALSE(opt.has_value());
}

TEST(FlatGeneratorTest, FailsWhenBiomeUnknown) {
    flat::FlatSpec spec;
    spec.biome = "minecraft:not_a_biome";
    auto opt = flat::generate(0, 0, spec, mock_block, mock_biome);
    EXPECT_FALSE(opt.has_value());
}

TEST(FlatGeneratorTest, AirFallsBackToZero) {
    // Resolver returns nullopt for air; generator must use state 0 (wiki guarantee)
    auto no_air_resolver = [](std::string_view name) -> std::optional<std::int32_t> {
        if (name == "minecraft:bedrock")     return 7;
        if (name == "minecraft:dirt")        return 13;
        if (name == "minecraft:grass_block") return 42;
        return std::nullopt;  // air intentionally missing
        };
    flat::FlatSpec spec;
    auto opt = flat::generate(0, 0, spec, no_air_resolver, mock_biome);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->sections[0].block_palette[0], 0);  // air = 0 fallback
}