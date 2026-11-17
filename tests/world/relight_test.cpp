#include "world/block_world.hpp"
#include "world/chunk_decoder.hpp"
#include "world/chunk_encoder.hpp"
#include "world/light_engine.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <tuple>

namespace {

    constexpr std::int32_t kAir = 1;
    constexpr std::int32_t kStone = 2;
    // The test world is one chunk of two sections: stone below y=16, open sky above it.
    constexpr std::int32_t kSurfaceTop = 15;
    constexpr std::int32_t kWorldTop = 31;

    using EditMap = std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, std::int32_t>;

    terila::world::Chunk make_world() {
        terila::world::Chunk c;
        c.min_y = 0;
        c.section_count = 2;
        c.sections.resize(2);
        c.sections[0].block_palette = { kStone };
        c.sections[0].block_bits = 0;
        c.sections[0].block_count = 4096;
        c.sections[1].block_palette = { kAir };
        c.sections[1].block_bits = 0;
        return c;
    }

    terila::world::LightRules make_rules() {
        terila::world::LightRules rules;
        rules.set(kAir, terila::world::LightBlock{ 0, 0, true });
        rules.set(kStone, terila::world::LightBlock{ 0, 15, false });
        return rules;
    }

    std::int32_t generated_state(std::int32_t, std::int32_t y, std::int32_t) {
        return y <= kSurfaceTop ? kStone : kAir;
    }

    // Lights the chunk over the edits the player made, exactly like the server does after a
    // block change.
    void relight(terila::world::Chunk& chunk, const terila::world::LightRules& rules,
        const EditMap& edits) {
        // The server writes the edits into the chunk before lighting it again; the test does
        // the same, so the light engine sees them from the inside too.
        for (const auto& [pos, state] : edits) {
            const auto& [x, y, z] = pos;
            if (x < 0 || x > 15 || z < 0 || z > 15) {
                continue;
            }
            const int local_y = y - chunk.min_y;
            if (local_y < 0 || local_y >= chunk.section_count * 16) {
                continue;
            }
            terila::world::section_set(chunk.sections[static_cast<std::size_t>(local_y / 16)],
                ((local_y % 16) * 16 + z) * 16 + x, state, kAir);
        }

        const auto state = [&edits](std::int32_t x, std::int32_t y, std::int32_t z) {
            const auto found = edits.find(std::make_tuple(x, y, z));
            return found == edits.end() ? generated_state(x, y, z) : found->second;
        };
        const auto top = [&rules, &state](std::int32_t x, std::int32_t z) {
            return terila::world::edited_sky_top(0, kWorldTop, x, z,
                [](std::int32_t, std::int32_t) { return kSurfaceTop; },
                state,
                [&rules](std::int32_t s) { return rules.of(s).sky_down; });
        };
        terila::world::LightEngine::compute(chunk, rules, top, state);
    }

    // The encoder's packing, read back for one cell.
    int sky_level(const terila::world::Chunk& c, std::int32_t x, std::int32_t y, std::int32_t z) {
        const int section = (y - c.min_y) / 16;
        const int bit = section + 1;
        const auto has = [](const std::vector<std::int64_t>& mask, int index) {
            return ((static_cast<std::uint64_t>(mask[static_cast<std::size_t>(index / 64)]) >>
                (index % 64)) & 1ULL) != 0;
        };
        if (!has(c.light.sky_y_mask, bit)) {
            return 0;
        }

        std::size_t array = 0;
        for (int s = 0; s < section; ++s) {
            if (has(c.light.sky_y_mask, s + 1)) {
                ++array;
            }
        }
        if (array >= c.light.sky_updates.size()) {
            return 0;
        }

        const int cell = (((y - c.min_y) % 16) * 16 + (z & 15)) * 16 + (x & 15);
        const auto& packed = c.light.sky_updates[array];
        const auto byte = packed[static_cast<std::size_t>(cell / 2)];
        return (cell & 1) ? (byte >> 4) & 0x0F : byte & 0x0F;
    }

} // namespace

TEST(RelightTest, ThePristineColumnIsLitDownToItsSurface) {
    auto chunk = make_world();
    const auto rules = make_rules();
    relight(chunk, rules, {});

    EXPECT_EQ(sky_level(chunk, 0, kWorldTop, 0), 15);
    EXPECT_EQ(sky_level(chunk, 0, 16, 0), 15);
    // The surface block itself is solid, so it carries no sky light of its own.
    EXPECT_EQ(sky_level(chunk, 0, kSurfaceTop, 0), 0);
}

TEST(RelightTest, ADugOutBlockLetsTheSkyIn) {
    auto chunk = make_world();
    const auto rules = make_rules();
    relight(chunk, rules, {});

    // This is the black hole from the bug: with the generator's light the dug-out cell keeps
    // the darkness it had as stone.
    ASSERT_EQ(sky_level(chunk, 0, kSurfaceTop, 0), 0);

    const EditMap edits{ { std::make_tuple(0, kSurfaceTop, 0), kAir } };
    relight(chunk, rules, edits);

    EXPECT_EQ(sky_level(chunk, 0, kSurfaceTop, 0), 15);
    // Everything deeper in the column is still covered by the blocks above it.
    EXPECT_EQ(sky_level(chunk, 0, kSurfaceTop - 1, 0), 0);
}

TEST(RelightTest, APlacedBlockTakesTheSkyAwayAgain) {
    auto chunk = make_world();
    const auto rules = make_rules();

    const EditMap edits{ { std::make_tuple(0, 16, 0), kStone } };
    relight(chunk, rules, edits);

    // The column now ends at the placed block instead of the generated surface.
    EXPECT_EQ(sky_level(chunk, 0, 16, 0), 0);
    EXPECT_EQ(sky_level(chunk, 0, 17, 0), 15);
    EXPECT_EQ(sky_level(chunk, 0, 15, 0), 0);
}

// The world on disk: a chunk saved before the light was recomputed keeps the darkness of
// its old blocks, and streaming it back is what made a restart show black holes.
TEST(RelightTest, AStoredChunkWithStaleLightIsRelitWhenItIsServed) {
    auto stored = make_world();
    const auto rules = make_rules();
    relight(stored, rules, {});   // light of the untouched column, as an old save has it

    // Dig a block in the stored copy without relighting it, exactly like the old build.
    terila::world::section_set(stored.sections[0], ((kSurfaceTop % 16) * 16 + 0) * 16 + 0,
        kAir, kAir);
    const auto payload = terila::world::encode::encode(stored);

    auto loaded = terila::world::decode::decode(payload.data(), payload.size(), 2);
    ASSERT_TRUE(loaded.has_value());
    // The decoder is told the section count; the caller owns where the world starts.
    loaded->min_y = 0;
    ASSERT_EQ(sky_level(*loaded, 0, kSurfaceTop, 0), 0);   // the black hole

    const EditMap edits{ { std::make_tuple(0, kSurfaceTop, 0), kAir } };
    relight(*loaded, rules, edits);

    EXPECT_EQ(sky_level(*loaded, 0, kSurfaceTop, 0), 15);
}

TEST(RelightTest, DiggingIntoAColumnLightsTheCellAndItsNeighbours) {
    auto chunk = make_world();
    const auto rules = make_rules();

    // A one-block hole in the surface: the cell itself is full daylight, and the stone beside
    // it stays dark because a solid block costs the whole level.
    const EditMap edits{ { std::make_tuple(4, kSurfaceTop, 4), kAir } };
    relight(chunk, rules, edits);

    EXPECT_EQ(sky_level(chunk, 4, kSurfaceTop, 4), 15);
    EXPECT_EQ(sky_level(chunk, 5, kSurfaceTop, 4), 0);
}
