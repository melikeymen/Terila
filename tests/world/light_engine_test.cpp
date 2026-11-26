#include <gtest/gtest.h>

#include "data/block_light_loader.hpp"
#include "data/input_store.hpp"
#include "data/registry_data.hpp"
#include "world/light_engine.hpp"
#include "world/terrain_biomes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

// The engine clips the work to a band and only seeds the cells light can leave from. Both
// are optimisations, so what they produce has to be the game's own answer: every cell holds
// its source level, or one less than its brightest neighbour, over the whole height and the
// whole halo. The reference below is that fill written the slow, obvious way.
TEST(LightEngineTest, TheFieldEqualsAPlainFloodFill) {
    constexpr int kRadius = world::LightEngine::kRadius;
    constexpr int kSide = 16 + 2 * kRadius;
    constexpr int kHeight = 32;
    constexpr std::int32_t kGlass = 5;

    const auto block = [](int x, int y, int z) -> std::int32_t {
        const auto hash = static_cast<std::uint32_t>(
            (static_cast<std::uint32_t>(x + 40) * 73856093u) ^
            (static_cast<std::uint32_t>(z + 40) * 19349663u));
        const int ground = 6 + static_cast<int>(hash % 7u);
        if (y <= ground) {
            const bool cave = y >= 3 && y < ground && ((hash >> 8) % 3u) == 0;
            return cave ? kAir : kStone;
        }
        if (y <= 14 && ((x + z) % 7) == 0) return kWater;
        if (y == 20 && x >= 2 && x <= 6 && z >= 2 && z <= 6) return kGlass;
        if (y == ground + 1 && ((hash >> 12) % 11u) == 0) return kTorch;
        return kAir;
    };
    // A block the sky passes through: air, and the glass roof over the corner.
    const auto open_to_sky = [](std::int32_t state) {
        return state == kAir || state == kGlass || state == kTorch;
    };
    const auto sky_top = [&](int x, int z) {
        int y = kHeight - 1;
        while (y >= 0 && open_to_sky(block(x, y, z))) {
            --y;
        }
        return y;
    };

    world::LightRules rules = standard_rules();
    rules.set(kGlass, world::LightBlock{ 0, 1, true });

    auto chunk = make_chunk(0, 0, 2, block);
    world::LightEngine::compute(chunk, rules, sky_top,
        [&](int x, int y, int z) { return block(x, y, z); });

    const auto index = [](int x, int y, int z) {
        return static_cast<std::size_t>(
            (static_cast<std::size_t>(y) * kSide + static_cast<std::size_t>(z + kRadius)) *
            kSide + static_cast<std::size_t>(x + kRadius));
    };
    const auto inside = [](int x, int y, int z) {
        return x >= -kRadius && x < 16 + kRadius && z >= -kRadius && z < 16 + kRadius &&
            y >= 0 && y < kHeight;
    };

    const auto flood = [&](std::vector<std::uint8_t>& levels) {
        static constexpr int dx[6] = { 1, -1, 0, 0, 0, 0 };
        static constexpr int dy[6] = { 0, 0, 1, -1, 0, 0 };
        static constexpr int dz[6] = { 0, 0, 0, 0, 1, -1 };
        std::vector<std::size_t> queue;
        for (std::size_t cell = 0; cell < levels.size(); ++cell) {
            if (levels[cell] > 1) queue.push_back(cell);
        }
        for (std::size_t head = 0; head < queue.size(); ++head) {
            const std::size_t cell = queue[head];
            const int x = static_cast<int>(cell % kSide) - kRadius;
            const int z = static_cast<int>((cell / kSide) % kSide) - kRadius;
            const int y = static_cast<int>(cell / (kSide * kSide));
            for (int direction = 0; direction < 6; ++direction) {
                const int nx = x + dx[direction];
                const int ny = y + dy[direction];
                const int nz = z + dz[direction];
                if (!inside(nx, ny, nz)) {
                    continue;
                }
                const int cost = std::max(1, static_cast<int>(
                    rules.of(block(nx, ny, nz)).opacity));
                const int next = static_cast<int>(levels[cell]) - cost;
                if (next <= 0) {
                    continue;
                }
                auto& target = levels[index(nx, ny, nz)];
                if (target >= next) {
                    continue;
                }
                target = static_cast<std::uint8_t>(next);
                queue.push_back(index(nx, ny, nz));
            }
        }
    };

    std::vector<std::uint8_t> sky(static_cast<std::size_t>(kSide) * kSide * kHeight, 0);
    for (int z = -kRadius; z < 16 + kRadius; ++z) {
        for (int x = -kRadius; x < 16 + kRadius; ++x) {
            for (int y = std::max(0, sky_top(x, z) + 1); y < kHeight; ++y) {
                sky[index(x, y, z)] = 15;
            }
        }
    }
    flood(sky);

    std::vector<std::uint8_t> light(static_cast<std::size_t>(kSide) * kSide * kHeight, 0);
    for (int y = 0; y < kHeight; ++y) {
        for (int z = -kRadius; z < 16 + kRadius; ++z) {
            for (int x = -kRadius; x < 16 + kRadius; ++x) {
                light[index(x, y, z)] =
                    static_cast<std::uint8_t>(rules.of(block(x, y, z)).emission);
            }
        }
    }
    flood(light);

    int sky_mismatches = 0;
    int light_mismatches = 0;
    int reader_mismatches = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                if (level_at(chunk, true, x, y, z) != sky[index(x, y, z)]) ++sky_mismatches;
                if (level_at(chunk, false, x, y, z) != light[index(x, y, z)]) {
                    ++light_mismatches;
                }
                // The reader the spawner uses has to see the same numbers the client gets.
                if (world::light_level_at(chunk, x, y, z, world::LightLayer::Sky) !=
                    level_at(chunk, true, x, y, z)) {
                    ++reader_mismatches;
                }
                if (world::light_level_at(chunk, x, y, z, world::LightLayer::Block) !=
                    level_at(chunk, false, x, y, z)) {
                    ++reader_mismatches;
                }
            }
        }
    }
    EXPECT_EQ(reader_mismatches, 0) << "the stored light reader disagrees with the packet";
    EXPECT_EQ(sky_mismatches, 0) << "the sky field has to be the plain fill's own answer";
    EXPECT_EQ(light_mismatches, 0) << "the block light field has to be the plain fill's own answer";
}

// The light a chunk is drawn with comes from the block data, not from a short list of blocks:
// every state of every block has to answer with the game's own emission and dampening.
TEST(LightEngineTest, TheBlockDataAnswersTheLightOfEveryState) {
    static const bool available = [] {
        std::error_code ec;
        for (const char* prefix : { "input/", "", "../input/", "../../input/",
                 "../../../input/" }) {
            const std::filesystem::path root(prefix);
            if (std::filesystem::is_regular_file(root / "blocks" / "minecraft.json", ec) &&
                !ec) {
                terila::data::input::mount(root.string());
                return true;
            }
            ec.clear();
        }
        return false;
        }();
    if (!available) {
        GTEST_SKIP() << "input/ is not part of this checkout";
    }

    terila::data::RegistryData data;
    terila::data::load_block_light(data, "blocks/minecraft.json");
    ASSERT_FALSE(data.block_light.empty());
    // Every state id the dump writes carries an answer; a missing one is drawn as opaque and
    // would put a window in the dark.
    EXPECT_GT(data.block_light.size(), 30000u);

    const auto info_of = [&data](std::string_view name) {
        static const auto ids = [] {
            std::unordered_map<std::string, std::int32_t> table;
            std::string text;
            if (!terila::data::input::read_text("blocks/state_ids.json", text)) {
                return table;
            }
            const auto j = nlohmann::json::parse(text);
            for (const auto& entry : j["data"]) {
                if (entry.contains("default_state_id")) {
                    table[entry["id"].get<std::string>()] =
                        entry["default_state_id"].get<std::int32_t>();
                }
                else if (entry.contains("min_state_id")) {
                    table[entry["id"].get<std::string>()] =
                        entry["min_state_id"].get<std::int32_t>();
                }
            }
            return table;
            }();
        const auto state = ids.find(std::string(name));
        if (state == ids.end()) return terila::data::BlockLightInfo{};
        const auto found = data.block_light.find(state->second);
        return found == data.block_light.end() ? terila::data::BlockLightInfo{} : found->second;
    };

    EXPECT_EQ(info_of("minecraft:torch").emission, 14);
    EXPECT_EQ(info_of("minecraft:glowstone").emission, 15);
    EXPECT_EQ(info_of("minecraft:sea_lantern").emission, 15);
    EXPECT_EQ(info_of("minecraft:stone").dampening, 15);
    EXPECT_EQ(info_of("minecraft:stone").emission, 0);
    EXPECT_LT(info_of("minecraft:glass").dampening, 15) << "a window lets light through";
    EXPECT_LT(info_of("minecraft:water").dampening, 15);
    EXPECT_LT(info_of("minecraft:oak_leaves").dampening, 15);
    EXPECT_EQ(info_of("minecraft:air").dampening, 0);
    EXPECT_TRUE(info_of("minecraft:stone").collides);
    EXPECT_FALSE(info_of("minecraft:air").collides);
}
