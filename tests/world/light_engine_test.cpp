#include <gtest/gtest.h>

#include "world/light_engine.hpp"
#include "world/terrain_biomes.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace world = terila::world;
namespace terrain = terila::world::terrain;

namespace {

    constexpr std::int32_t kAir = 1;
    constexpr std::int32_t kStone = 2;
    constexpr std::int32_t kWater = 3;
    constexpr std::int32_t kTorch = 4;

    using BlockFn = std::function<std::int32_t(int, int, int)>;

    // A chunk whose sections hold absolute state ids: the engine only reads them, so the
    // direct palette form keeps the fixture small.
    world::Chunk make_chunk(std::int32_t cx, std::int32_t cz, int section_count,
        const BlockFn& block) {
        world::Chunk chunk;
        chunk.x = cx;
        chunk.z = cz;
        chunk.min_y = 0;
        chunk.section_count = section_count;
        chunk.sections.resize(static_cast<std::size_t>(section_count));

        for (int sy = 0; sy < section_count; ++sy) {
            auto& section = chunk.sections[static_cast<std::size_t>(sy)];
            section.block_bits = 9;   // direct states
            section.block_palette.clear();
            section.block_states.assign(4096, 0);
            for (int i = 0; i < 4096; ++i) {
                const int y = sy * 16 + (i / 256);
                const int z = (i / 16) % 16;
                const int x = i % 16;
                section.block_states[static_cast<std::size_t>(i)] = block(x, y, z);
            }
        }
        return chunk;
    }

    world::LightRules standard_rules() {
        world::LightRules rules;
        rules.set(kAir, world::LightBlock{ 0, 0, true });
        rules.set(kStone, world::LightBlock{ 0, 15, false });
        rules.set(kWater, world::LightBlock{ 0, 1, false });
        rules.set(kTorch, world::LightBlock{ 14, 0, true });
        return rules;
    }

    // Reads a level back out of the packet form: the masks say which sections carry an
    // array, and the arrays follow in ascending section order.
    int level_at(const world::Chunk& chunk, bool sky, int x, int y, int z) {
        const int section = (y - chunk.min_y) / 16;
        if (section < 0 || section >= chunk.section_count) {
            return 0;
        }
        const auto& mask = sky ? chunk.light.sky_y_mask : chunk.light.block_y_mask;
        const auto& arrays = sky ? chunk.light.sky_updates : chunk.light.block_updates;

        const int bit = section + 1;   // bit i addresses section i-1
        const auto word = static_cast<std::size_t>(bit / 64);
        if (word >= mask.size() || (mask[word] & (1LL << (bit % 64))) == 0) {
            return 0;
        }

        std::size_t index = 0;
        for (int b = 0; b < bit; ++b) {
            const auto w = static_cast<std::size_t>(b / 64);
            if (w < mask.size() && (mask[w] & (1LL << (b % 64))) != 0) {
                ++index;
            }
        }
        if (index >= arrays.size()) {
            return 0;
        }

        const auto& array = arrays[index];
        const int local_y = (y - chunk.min_y) % 16;
        const int cell = (local_y * 16 + z) * 16 + x;
        const auto byte = static_cast<std::size_t>(cell / 2);
        if (byte >= array.size()) {
            return 0;
        }
        return (cell % 2 == 0) ? (array[byte] & 0x0F) : ((array[byte] >> 4) & 0x0F);
    }

} // namespace

TEST(LightEngineTest, SunlightReachesTheGroundAndStopsThere) {
    const auto block = [](int, int y, int) { return y < 4 ? kStone : kAir; };
    auto chunk = make_chunk(0, 0, 2, block);

    world::LightEngine::compute(chunk, standard_rules(),
        [](int, int) { return 3; },
        [&](int, int y, int) { return block(0, y, 0); });

    EXPECT_EQ(level_at(chunk, true, 8, 31, 8), 15);   // open sky
    EXPECT_EQ(level_at(chunk, true, 8, 4, 8), 15);    // first air above the floor
    EXPECT_EQ(level_at(chunk, true, 8, 3, 8), 0);     // the floor itself is dark
    EXPECT_EQ(level_at(chunk, false, 8, 4, 8), 0);    // no light source anywhere
}

TEST(LightEngineTest, WaterCostsOneLevelPerBlock) {
    const auto block = [](int, int y, int) {
        if (y <= 8) return kStone;
        if (y >= 10 && y <= 12) return kWater;
        return kAir;
    };
    auto chunk = make_chunk(0, 0, 2, block);

    world::LightEngine::compute(chunk, standard_rules(),
        [](int, int) { return 12; },
        [&](int, int y, int) { return block(0, y, 0); });

    EXPECT_EQ(level_at(chunk, true, 8, 13, 8), 15);
    EXPECT_EQ(level_at(chunk, true, 8, 12, 8), 14);
    EXPECT_EQ(level_at(chunk, true, 8, 11, 8), 13);
    EXPECT_EQ(level_at(chunk, true, 8, 10, 8), 12);
    EXPECT_EQ(level_at(chunk, true, 8, 9, 8), 11);
    EXPECT_EQ(level_at(chunk, true, 8, 8, 8), 0);   // the sea floor stays dark
}

TEST(LightEngineTest, BlockLightFallsOffOneLevelPerBlock) {
    const auto block = [](int x, int y, int z) {
        if (y <= 4 || y >= 8) return kStone;
        if (x == 8 && y == 6 && z == 8) return kTorch;
        return kAir;
    };
    auto chunk = make_chunk(0, 0, 2, block);

    world::LightEngine::compute(chunk, standard_rules(),
        [](int, int) { return 31; },
        [](int, int, int) { return kStone; });

    EXPECT_EQ(level_at(chunk, false, 8, 6, 8), 14);   // the torch itself
    EXPECT_EQ(level_at(chunk, false, 9, 6, 8), 13);
    EXPECT_EQ(level_at(chunk, false, 10, 6, 8), 12);
    EXPECT_EQ(level_at(chunk, false, 8, 5, 8), 13);   // down as well
    EXPECT_EQ(level_at(chunk, true, 8, 6, 8), 0);     // the roof keeps the sun out
}

// The halo is what makes chunk borders seamless: the light of a chunk is computed with
// the blocks around it, so a cave crossing the border is lit from the other side too.
TEST(LightEngineTest, LightCrossesAChunkBorder) {
    // A cave slab running through both chunks, with one shaft open to the sky in the first.
    const auto block = [](int wx, int wy, int wz) {
        if (wy == 6 || wy == 7) return kAir;           // the cave floor, both chunks
        if (wx == 8 && wz == 8) return kAir;           // the shaft, straight up to the sky
        if (wy >= 8 || wy <= 5) return kStone;
        return kAir;
    };

    auto first = make_chunk(0, 0, 2, [&](int x, int y, int z) { return block(x, y, z); });
    auto second = make_chunk(1, 0, 2, [&](int x, int y, int z) { return block(x + 16, y, z); });

    const auto sample = [&](int wx, int wy, int wz) { return block(wx, wy, wz); };
    const auto top = [](int wx, int wz) { return (wx == 8 && wz == 8) ? 5 : 31; };
    world::LightEngine::compute(first, standard_rules(), top, sample);
    world::LightEngine::compute(second, standard_rules(), top, sample);

    const int inside = level_at(first, true, 15, 6, 8);
    const int across = level_at(second, true, 0, 6, 8);
    EXPECT_GT(inside, 0) << "the cave is lit on the first side of the border";
    EXPECT_GT(across, 0) << "the light continues into the next chunk";
    EXPECT_EQ(inside, across + 1) << "one level per block across the border";
}

// A light source a player places has to be in the rules the server lights with, otherwise a
// chunk holding a shroomlight is saved and streamed back with the lamp dark.
TEST(LightEngineTest, PlacedLightSourcesAreRegisteredAsEmitters) {
    const terrain::IdResolver resolver = [](std::string_view name) -> std::optional<std::int32_t> {
        if (name == "minecraft:shroomlight") return 900;
        if (name == "minecraft:torch") return 901;
        return std::nullopt;
    };
    world::LightRules rules;
    terrain::add_placed_light_sources(rules, resolver);

    EXPECT_EQ(15, rules.of(900).emission);
    EXPECT_EQ(14, rules.of(901).emission);
    EXPECT_TRUE(rules.has_emitters());
}

// The band the pass covers follows the chunk: a lamp in a cave below the sky has to be lit
// even though nothing above it is dark.
TEST(LightEngineTest, AChunkWithALampIsLitBelowTheSky) {
    // A lamp in a small room under the surface: the sky never reaches it, so any light at
    // all comes from the block light pass.
    const auto block = [](int x, int y, int z) {
        if (x == 8 && y == 5 && z == 8) return kTorch;
        if (y >= 4 && y <= 6 && x >= 6 && x <= 10 && z >= 6 && z <= 10) return kAir;
        return kStone;
    };
    auto chunk = make_chunk(0, 0, 2, block);
    world::LightEngine::compute(chunk, standard_rules(),
        [](int, int) { return 7; },
        [&](int x, int y, int z) { return block(x, y, z); });

    EXPECT_GT(level_at(chunk, false, 8, 5, 8), 0);
    EXPECT_GT(level_at(chunk, false, 9, 5, 8), 0);
    EXPECT_LT(level_at(chunk, false, 10, 5, 8), level_at(chunk, false, 9, 5, 8));
}
