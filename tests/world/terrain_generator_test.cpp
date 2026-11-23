#include <gtest/gtest.h>

#include "world/block_cache.hpp"
#include "world/chunk_jobs.hpp"
#include "data/input_store.hpp"
#include "world/light_engine.hpp"
#include "world/world_context.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <utility>
#include <unordered_map>
#include <vector>
#include "world/terrain_generator.hpp"
#include "world/world_generator.hpp"
#include "world/chunk_encoder.hpp"
#include "world/chunk_decoder.hpp"
#include "world/block_world.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

using namespace terila::world;

// Skips a test whose worldgen settings are not on disk.
#define REQUIRE_VANILLA_DATA()                                              if (!vanilla_data_available()) {                                            GTEST_SKIP() << "input/ with the worldgen settings is missing";     }

namespace {

    // Deliberately non-sequential ids so a hardcoded palette would fail.
    std::optional<std::int32_t> mock_block(std::string_view name) {
        if (name == "minecraft:air")           return 0;
        if (name == "minecraft:bedrock")       return 7;
        if (name == "minecraft:stone")         return 1;
        if (name == "minecraft:dirt")          return 13;
        if (name == "minecraft:grass_block")   return 42;
        if (name == "minecraft:sand")          return 121;
        if (name == "minecraft:sandstone")     return 677;
        if (name == "minecraft:gravel")        return 127;
        if (name == "minecraft:water")         return 89;
        if (name == "minecraft:snow")          return 8589;
        if (name == "minecraft:oak_log")       return 140;
        if (name == "minecraft:oak_leaves")    return 294;
        if (name == "minecraft:spruce_log")    return 143;
        if (name == "minecraft:spruce_leaves") return 295;
        if (name == "minecraft:acacia_log")    return 152;
        if (name == "minecraft:acacia_leaves") return 300;
        if (name == "minecraft:cactus")        return 8599;
        if (name == "minecraft:short_grass")   return 8600;
        if (name == "minecraft:fern")          return 8601;
        if (name == "minecraft:dandelion")     return 8602;
        if (name == "minecraft:poppy")         return 8603;
        if (name == "minecraft:dead_bush")     return 8604;
        if (name == "minecraft:coarse_dirt")   return 14;
        if (name == "minecraft:red_sand")      return 122;
        if (name == "minecraft:terracotta")    return 678;
        if (name == "minecraft:ice")           return 90;
        if (name == "minecraft:birch_log")     return 141;
        if (name == "minecraft:birch_leaves")  return 296;
        if (name == "minecraft:jungle_log")    return 154;
        if (name == "minecraft:jungle_leaves") return 302;
        if (name == "minecraft:coal_ore")      return 2;
        if (name == "minecraft:copper_ore")    return 3;
        if (name == "minecraft:iron_ore")      return 4;
        if (name == "minecraft:gold_ore")      return 5;
        if (name == "minecraft:redstone_ore")  return 6;
        if (name == "minecraft:lapis_ore")     return 8;
        if (name == "minecraft:diamond_ore")   return 9;
        if (name == "minecraft:granite")       return 21;
        if (name == "minecraft:diorite")       return 22;
        if (name == "minecraft:andesite")      return 23;
        if (name == "minecraft:tuff")          return 24;
        if (name == "minecraft:clay")          return 25;
        if (name == "minecraft:deepslate")     return 26;
        if (name == "minecraft:obsidian")      return 30;
        if (name == "minecraft:netherrack")    return 31;
        if (name == "minecraft:stone_bricks")  return 32;
        return std::nullopt;
    }

    std::optional<std::int32_t> mock_biome(std::string_view name) {
        if (name == "minecraft:ocean")          return 1;
        if (name == "minecraft:beach")          return 2;
        if (name == "minecraft:plains")         return 3;
        if (name == "minecraft:forest")         return 4;
        if (name == "minecraft:desert")         return 5;
        if (name == "minecraft:snowy_plains")   return 6;
        if (name == "minecraft:taiga")          return 7;
        if (name == "minecraft:savanna")        return 8;
        if (name == "minecraft:stony_peaks")    return 9;
        if (name == "minecraft:deep_ocean")     return 10;
        if (name == "minecraft:frozen_ocean")   return 11;
        if (name == "minecraft:stony_shore")    return 12;
        if (name == "minecraft:snowy_taiga")    return 13;
        if (name == "minecraft:windswept_hills") return 14;
        if (name == "minecraft:meadow")         return 15;
        if (name == "minecraft:birch_forest")   return 16;
        if (name == "minecraft:swamp")          return 17;
        if (name == "minecraft:jungle")         return 18;
        if (name == "minecraft:badlands")       return 19;
        return std::nullopt;
    }

    // The world generator reads the vanilla worldgen settings from input/, an extracted
    // data directory the repository does not carry, so a checkout without it (CI) skips the
    // tests that need it instead of building a generator that cannot generate.
    bool vanilla_data_available() {
        static const bool available = [] {
            std::error_code ec;
            for (const char* prefix : { "input/", "", "../input/", "../../input/",
                     "../../../input/" }) {
                const std::filesystem::path root(prefix);
                if (std::filesystem::is_regular_file(root / "worldgen" / "noise_settings.json",
                        ec) && !ec) {
                    // The generator reads through the input store as well, so pointing it at
                    // the directory makes the build tree independent of the working one.
                    terila::data::input::mount(root.string());
                    return true;
                }
                ec.clear();
            }
            return false;
            }();
        return available;
    }

    terrain::Generator make_generator(std::uint32_t seed = 1337) {
        terrain::Generator generator;
        EXPECT_TRUE(generator.init(-64, 24, seed, mock_block, mock_biome));
        return generator;
    }

    std::int32_t section_block(const Chunk& chunk, std::int32_t x, std::int32_t y,
        std::int32_t z) {
        const int local_y = y - chunk.min_y;
        if (local_y < 0 || local_y >= chunk.section_count * 16) {
            return -1;
        }
        const auto& section = chunk.sections[static_cast<std::size_t>(local_y / 16)];
        return section_state(section, (local_y % 16) * 256 + (z & 15) * 16 + (x & 15));
    }

} // namespace

TEST(TerrainGeneratorTest, FailsWhenABlockIsUnknown) {
    REQUIRE_VANILLA_DATA();
    terrain::Generator generator;
    const auto missing = [](std::string_view name) -> std::optional<std::int32_t> {
        if (name == "minecraft:cactus") return std::nullopt;
        return mock_block(name);
        };
    EXPECT_FALSE(generator.init(-64, 24, 1337, missing, mock_biome));
}

TEST(TerrainGeneratorTest, FailsWhenABiomeIsUnknown) {
    REQUIRE_VANILLA_DATA();
    terrain::Generator generator;
    const auto missing = [](std::string_view name) -> std::optional<std::int32_t> {
        if (name == "minecraft:taiga") return std::nullopt;
        return mock_biome(name);
        };
    EXPECT_FALSE(generator.init(-64, 24, 1337, mock_block, missing));
}

TEST(TerrainGeneratorTest, SameSeedGeneratesTheSameChunk) {
    REQUIRE_VANILLA_DATA();
    const auto first = make_generator(1337);
    const auto second = make_generator(1337);
    EXPECT_EQ(encode::encode(first.generate(3, -2)), encode::encode(second.generate(3, -2)));
}

TEST(TerrainGeneratorTest, DifferentSeedsGenerateDifferentTerrain) {
    REQUIRE_VANILLA_DATA();
    const auto first = make_generator(1337);
    const auto second = make_generator(7331);
    EXPECT_NE(encode::encode(first.generate(0, 0)), encode::encode(second.generate(0, 0)));
}

TEST(TerrainGeneratorTest, ColumnHasBedrockUnderSurfaceAndAirAbove) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    const auto chunk = generator.generate(0, 0);
    EXPECT_EQ(section_block(chunk, 8, generator.min_y(), 8), 7);   // bedrock

    const int surface = generator.surface_height(8, 8);
    EXPECT_EQ(section_block(chunk, 8, surface, 8), generator.state_at(8, surface, 8));
    // The top of the world is always air, whatever the column below it is.
    EXPECT_EQ(section_block(chunk, 8, generator.min_y() + 24 * 16 - 1, 8), 0);
}

TEST(TerrainGeneratorTest, UnderwaterColumnsHoldWaterAtSeaLevel) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    const auto chunk = generator.generate(0, 0);

    int checked = 0;
    for (std::int32_t z = 0; z < 16 && checked < 8; ++z) {
        for (std::int32_t x = 0; x < 16 && checked < 8; ++x) {
            if (generator.surface_height(x, z) >= generator.sea_level()) {
                continue;
            }
            EXPECT_EQ(section_block(chunk, x, generator.sea_level(), z), 89);
            ++checked;
        }
    }
}

TEST(TerrainGeneratorTest, StateAtMatchesTheGeneratedChunk) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    for (const auto& position : { std::pair{ 0, 0 }, std::pair{ 4, -3 } }) {
        const auto chunk = generator.generate(position.first, position.second);
        int mismatches = 0;
        int reported = 0;
        // Sampled rather than exhaustive: state_at costs a full column per call, and the
        // generator is smooth enough that a strided sweep still catches disagreements.
        for (int local_y = 0; local_y < chunk.section_count * 16; local_y += 4) {
            const std::int32_t y = chunk.min_y + local_y;
            for (int z = 0; z < 16; z += 2) {
                for (int x = 0; x < 16; x += 2) {
                    const std::int32_t world_x = position.first * 16 + x;
                    const std::int32_t world_z = position.second * 16 + z;
                    const auto chunk_state = section_block(chunk, x, y, z);
                    const auto generated = generator.state_at(world_x, y, world_z);
                    if (chunk_state != generated) {
                        if (reported < 5) {
                            std::printf("   mismatch at (%d,%d,%d): chunk %d vs state_at %d, "
                                "surface %d, sea %d\n", world_x, y, world_z, chunk_state,
                                generated, generator.surface_height(world_x, world_z),
                                generator.sea_level());
                            ++reported;
                        }
                        ++mismatches;
                    }
                }
            }
        }
        EXPECT_EQ(mismatches, 0) << "chunk " << position.first << "," << position.second;
    }
}

TEST(TerrainGeneratorTest, ChunkSurvivesAnEncodeDecodeRoundTrip) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    const auto chunk = generator.generate(2, 5);
    const auto wire = encode::encode(chunk);
    auto decoded = decode::decode(wire.data(), wire.size(), 24);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->x, 2);
    EXPECT_EQ(decoded->z, 5);
    EXPECT_EQ(encode::encode(*decoded), wire);
}

TEST(WorldGeneratorTest, FlatWorldStreamsItsLayerCake) {
    terila::world::WorldGenerator generator;
    ASSERT_TRUE(generator.init(terila::world::WorldType::Flat, -64, 24, 0,
        mock_block, mock_biome));
    EXPECT_TRUE(generator.ready());
    EXPECT_EQ(generator.type(), terila::world::WorldType::Flat);

    const auto chunk = generator.generate(2, 3);
    EXPECT_EQ(chunk.x, 2);
    EXPECT_EQ(chunk.z, 3);
    ASSERT_EQ(chunk.sections.size(), 24u);

    const int surface = generator.surface_height(8, 8);
    EXPECT_EQ(surface, generator.min_y() + 4);
    EXPECT_EQ(section_block(chunk, 8, generator.min_y(), 8), 7);        // bedrock
    EXPECT_EQ(section_block(chunk, 8, generator.min_y() + 1, 8), 13);   // dirt
    EXPECT_EQ(section_block(chunk, 8, surface, 8), 42);                 // grass
    EXPECT_EQ(section_block(chunk, 8, surface + 1, 8), 0);              // air

    // The per-block lookup has to agree with the streamed chunk.
    EXPECT_EQ(generator.state_at(8, generator.min_y(), 8), 7);
    EXPECT_EQ(generator.state_at(8, surface, 8), 42);
    EXPECT_EQ(generator.state_at(8, surface + 1, 8), 0);
    EXPECT_EQ(generator.state_at(8, generator.min_y() - 1, 8), 0);
}

TEST(WorldGeneratorTest, LevelTypeNamesParse) {
    using terila::world::WorldType;
    EXPECT_EQ(terila::world::parse_world_type("flat"), WorldType::Flat);
    EXPECT_EQ(terila::world::parse_world_type("superflat"), WorldType::Flat);
    EXPECT_EQ(terila::world::parse_world_type("normal"), WorldType::Terrain);
    EXPECT_EQ(terila::world::parse_world_type("default"), WorldType::Terrain);
    EXPECT_FALSE(terila::world::parse_world_type("amplified").has_value());
}

TEST(WorldGeneratorTest, SpawnPointIsDryLandNearSeaLevel) {
    REQUIRE_VANILLA_DATA();
    terila::world::WorldGenerator generator;
    ASSERT_TRUE(generator.init(terila::world::WorldType::Terrain, -64, 24, 1337,
        mock_block, mock_biome));

    const auto spawn = generator.spawn_point();
    EXPECT_EQ(spawn.surface_y, generator.surface_height(spawn.x, spawn.z));
    EXPECT_GE(spawn.surface_y, generator.sea_level());

    // The whole point of the search: never a peak when the centre itself is one.
    const int center_level =
        std::abs(generator.surface_height(8, 8) - generator.sea_level());
    const int spawn_level = std::abs(spawn.surface_y - generator.sea_level());
    if (generator.surface_height(8, 8) >= generator.sea_level()) {
        EXPECT_LE(spawn_level, center_level);
    }
}

TEST(WorldGeneratorTest, FlatWorldSpawnsInItsCentreColumn) {
    terila::world::WorldGenerator generator;
    ASSERT_TRUE(generator.init(terila::world::WorldType::Flat, -64, 24, 0,
        mock_block, mock_biome));

    const auto spawn = generator.spawn_point();
    EXPECT_EQ(spawn.x, 8);
    EXPECT_EQ(spawn.z, 8);
    EXPECT_EQ(spawn.surface_y, generator.surface_height(8, 8));
}

namespace {
    // The map is mostly water around some seeds, so tests that need dry land look for it
    // instead of assuming the chunk at the origin has any.
    struct LandColumn {
        std::int32_t x = 0;
        std::int32_t z = 0;
        int height = 0;
        std::int32_t surface = 0;
        int biome = -1;
    };

    LandColumn find_column(const terrain::Generator& generator, bool grass_only) {
        for (std::int32_t radius = 0; radius <= 4096; radius += 16) {
            for (std::int32_t z = -radius; z <= radius; z += 8) {
                for (std::int32_t x = -radius; x <= radius; x += 8) {
                    if (radius > 0 && std::abs(x) != radius && std::abs(z) != radius) {
                        continue;
                    }
                    const int height = generator.surface_height(x, z);
                    const auto surface = generator.state_at(x, height, z);
                    // Grass under the sea cannot grow a tree, so only dry grassland counts.
                    const bool dry = height > generator.sea_level();
                    if ((surface == 42 && dry) || (!grass_only && dry)) {
                        return LandColumn{ x, z, height, surface, generator.biome_at(x, z) };
                    }
                }
            }
        }
        return LandColumn{};
    }

    bool is_plant(std::int32_t state) {
        return state == 140 || state == 294 || state == 143 || state == 295 ||
            state == 152 || state == 300 || state == 8599 || state == 141 ||
            state == 296 || state == 154 || state == 302;
    }

    bool is_ore(std::int32_t state) {
        return state == 2 || state == 3 || state == 4 || state == 5 || state == 6 ||
            state == 8 || state == 9;
    }
} // namespace

TEST(TerrainGeneratorTest, GeneratesVegetationSomewhere) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    const LandColumn land = find_column(generator, true);
    ASSERT_NE(land.biome, -1) << "no grassy column within 4096 blocks";

    // Grass does not imply trees: a single grassy column can sit in a biome without any.
    // Walk a few chunks around the first grassland and look for plants in any of them.
    int plants = 0;
    const std::int32_t base_x = land.x / 16;
    const std::int32_t base_z = land.z / 16;
    for (int i = 0; i < 9 && plants == 0; ++i) {
        const auto chunk = generator.generate(base_x + (i % 3) - 1, base_z + (i / 3) - 1);
        for (const auto& section : chunk.sections) {
            for (int k = 0; k < 4096; ++k) {
                if (is_plant(section_state(section, k))) {
                    ++plants;
                }
            }
        }
    }
    EXPECT_GT(plants, 0);
}

TEST(TerrainGeneratorTest, GroundCoverStandsOnTheSurface) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    const LandColumn land = find_column(generator, true);
    ASSERT_NE(land.biome, -1) << "no grassy column within 4096 blocks";

    const auto is_cover = [](std::int32_t state) { return state >= 8600 && state <= 8604; };

    int found = 0;
    int not_on_the_surface = 0;
    int on_the_surface = 0;
    int disagreements = 0;
    for (int i = 0; i < 9; ++i) {
        // A chunk holds the columns whose world coordinate divided by sixteen rounds down, so
        // the origin of a chunk west or north of zero must floor, not truncate.
        const auto chunk_of = [](std::int32_t v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); };
        const std::int32_t cx = chunk_of(land.x) + (i % 3) - 1;
        const std::int32_t cz = chunk_of(land.z) + (i / 3) - 1;
        const auto chunk = generator.generate(cx, cz);
        for (int s = 0; s < chunk.section_count; ++s) {
            const auto& section = chunk.sections[static_cast<std::size_t>(s)];
            const std::int32_t base_y = chunk.min_y + s * 16;
            for (int k = 0; k < 4096; ++k) {
                const std::int32_t state = section_state(section, k);
                if (!is_cover(state)) {
                    continue;
                }
                ++found;
                const std::int32_t x = cx * 16 + (k % 16);
                const std::int32_t z = cz * 16 + ((k / 16) % 16);
                const std::int32_t y = base_y + k / 256;
                // A plant stands in the block right above the column's surface, and the
                // convention has to hold for a single block read too, not only for the chunk.
                // The plant stands on the solid ground of its own column, never on air, water
                // or a leaf: a tuft floating over a pit is the failure this guards against.
                const std::int32_t below = section_block(chunk, x, y - 1, z);
                if (below == 0 || below == 89 || below == 294 || below == 295 || below == 296 ||
                    below == 300) {
                    if (not_on_the_surface < 5) {
                        std::printf("   cover %d at (%d,%d,%d) sits on %d, surface %d, biome %d\n",
                            state, x, y, z, below, generator.surface_height(x, z),
                            generator.biome_at(x, z));
                    }
                    ++not_on_the_surface;
                }
                // A single block read has to answer the plant as well. Only columns whose
                // surface the lookup agrees about are checked: a blended column height can sit
                // a cell away from the density surface, and on those few columns the chunk and
                // the lookup already disagree about the ground itself, plant or no plant.
                if (generator.surface_height(x, z) + 1 == y) {
                    ++on_the_surface;
                    if (generator.state_at(x, y, z) != state) {
                        if (disagreements < 5) {
                            std::printf(
                                "   lookup at (%d,%d,%d) is %d, chunk holds %d\n", x, y, z,
                                generator.state_at(x, y, z), state);
                        }
                        ++disagreements;
                    }
                }
            }
        }
    }
    EXPECT_GT(found, 0) << "nine chunks around a grassland grew no ground cover";
    EXPECT_EQ(not_on_the_surface, 0);
    EXPECT_GT(on_the_surface, 0) << "no plant landed on a column the lookup agrees about";
    EXPECT_EQ(disagreements, 0);
}

TEST(TerrainGeneratorTest, CarvesCavesUnderTheSurface) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    const LandColumn land = find_column(generator, false);
    ASSERT_NE(land.biome, -1) << "no land within 4096 blocks";

    int open_cells = 0;
    const auto chunk = generator.generate(land.x / 16, land.z / 16);
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            const int surface = generator.surface_height(land.x - land.x % 16 + x,
                land.z - land.z % 16 + z);
            for (int y = chunk.min_y + 6; y <= surface - 8; ++y) {
                if (section_block(chunk, x, y, z) == 0) {
                    ++open_cells;
                }
            }
        }
    }
    EXPECT_GT(open_cells, 0);
}

TEST(TerrainGeneratorTest, PlacesOreVeinsInTheDeep) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    const LandColumn land = find_column(generator, false);
    ASSERT_NE(land.biome, -1) << "no land within 4096 blocks";

    int ores = 0;
    const auto chunk = generator.generate(land.x / 16, land.z / 16);
    for (const auto& section : chunk.sections) {
        for (int i = 0; i < 4096; ++i) {
            if (is_ore(section_state(section, i))) {
                ++ores;
            }
        }
    }
    EXPECT_GT(ores, 0);
}

TEST(TerrainGeneratorTest, DeepslateFadesOutAboveYZero) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    const LandColumn land = find_column(generator, false);
    ASSERT_NE(land.biome, -1) << "no land within 4096 blocks";

    // Immediately above y=0 the game still writes mostly deepslate; by y=8 the fade is over.
    int deepslate_low = 0;
    int stone_low = 0;
    int deepslate_high = 0;
    for (int i = 0; i < 9; ++i) {
        const auto chunk = generator.generate(land.x / 16 + (i % 3), land.z / 16 + (i / 3));
        for (const auto& section : chunk.sections) {
            for (int k = 0; k < 4096; ++k) {
                const std::int32_t state = section_state(section, k);
                if (state != 1 && state != 26) {
                    continue;
                }
                const int y = chunk.min_y + static_cast<int>(&section - chunk.sections.data()) * 16
                    + k / 256;
                if (y >= 1 && y <= 4) {
                    (state == 26 ? deepslate_low : stone_low) += 1;
                }
                else if (y == 8) {
                    deepslate_high += state == 26 ? 1 : 0;
                }
            }
        }
    }
    SCOPED_TRACE("deepslate " + std::to_string(deepslate_low) + " stone " +
        std::to_string(stone_low) + " deepslate at y8 " + std::to_string(deepslate_high));
    EXPECT_GT(deepslate_low, stone_low);
    EXPECT_EQ(deepslate_high, 0);
}

TEST(TerrainGeneratorTest, StoneBlobsColourTheDeepFiller) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    const LandColumn land = find_column(generator, false);
    ASSERT_NE(land.biome, -1) << "no land within 4096 blocks";

    // The game colours a chunk of stone with granite, diorite, andesite and tuff blobs; a
    // chunk without them is stone where vanilla has anything else.
    std::array<int, 26> counts{};
    for (int i = 0; i < 9; ++i) {
        const auto chunk = generator.generate(land.x / 16 + (i % 3), land.z / 16 + (i / 3));
        for (const auto& section : chunk.sections) {
            for (int k = 0; k < 4096; ++k) {
                const std::int32_t state = section_state(section, k);
                if (state > 0 && state < static_cast<std::int32_t>(counts.size())) {
                    ++counts[static_cast<std::size_t>(state)];
                }
            }
        }
    }
    const int coloured = counts[21] + counts[22] + counts[23] + counts[24];
    SCOPED_TRACE("granite " + std::to_string(counts[21]) + " diorite " +
        std::to_string(counts[22]) + " andesite " + std::to_string(counts[23]) +
        " tuff " + std::to_string(counts[24]) + " clay " + std::to_string(counts[25]));
    EXPECT_GT(coloured, 400);
}

TEST(TerrainGeneratorTest, ABuiltStructureLandsOnTheSurface) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();

    // The ruined portal set places one structure per forty chunk cell; walk the cells around
    // the origin and build the one that sits on land.
    terila::world::worldgen::StructureSpread spread;
    spread.salt = 34222645;
    spread.spacing = 40;
    spread.separation = 15;

    int structure_blocks = 0;
    int built_chunks = 0;
    for (std::int32_t gz = -1; gz <= 1 && built_chunks < 2; ++gz) {
        for (std::int32_t gx = -1; gx <= 1 && built_chunks < 2; ++gx) {
            const auto site = terila::world::worldgen::spread_site(1337, spread, gx, gz);
            ASSERT_TRUE(site.valid);
            const std::int32_t centre_x = site.chunk_x * 16 + 8;
            const std::int32_t centre_z = site.chunk_z * 16 + 8;
            if (generator.surface_height(centre_x, centre_z) <= generator.sea_level()) {
                continue;
            }
            ++built_chunks;
            const auto chunk = generator.generate(site.chunk_x, site.chunk_z);
            for (const auto& section : chunk.sections) {
                for (int i = 0; i < 4096; ++i) {
                    const std::int32_t state = section_state(section, i);
                    if (state == 30 || state == 31 || state == 32) {
                        ++structure_blocks;
                    }
                }
            }
        }
    }
    if (built_chunks == 0) {
        GTEST_SKIP() << "no ruined portal site on land near the origin";
    }
    EXPECT_GT(structure_blocks, 0);
}

TEST(TerrainGeneratorTest, CoversManyBiomesAcrossTheMap) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    std::array<bool, 32> seen{};
    int distinct = 0;
    for (std::int32_t z = -2048; z <= 2048; z += 64) {
        for (std::int32_t x = -2048; x <= 2048; x += 64) {
            const int biome = generator.biome_at(x, z);
            if (biome >= 0 && biome < static_cast<int>(seen.size()) && !seen[biome]) {
                seen[static_cast<std::size_t>(biome)] = true;
                ++distinct;
            }
        }
    }
    EXPECT_GE(distinct, 6);
}

TEST(TerrainGeneratorTest, TerrainHasBothWaterAndHighGround) {
    REQUIRE_VANILLA_DATA();
    const auto generator = make_generator();
    int lowest = 1000;
    int highest = -1000;
    for (std::int32_t z = -1024; z <= 1024; z += 32) {
        for (std::int32_t x = -1024; x <= 1024; x += 32) {
            const int height = generator.surface_height(x, z);
            lowest = std::min(lowest, height);
            highest = std::max(highest, height);
        }
    }
    EXPECT_LT(lowest, generator.sea_level());
    EXPECT_GT(highest, generator.sea_level() + 20);
}

// Streaming a view distance generates batches of chunks on the worker pool. This prints what
// that buys over asking for the same columns one after another; it is a measurement, not an
// assertion, so it only runs when asked for.
TEST(ChunkJobsBench, BatchGenerationRunsInParallel) {
    REQUIRE_VANILLA_DATA();
    if (std::getenv("TERILA_BENCH") == nullptr) {
        GTEST_SKIP() << "set TERILA_BENCH=1 to run the chunk job benchmark";
    }

    auto& generator = WorldContext::shared().generator();
    ASSERT_TRUE(generator.init(WorldType::Terrain, -64, 24, 1337, mock_block, mock_biome));

    constexpr int kChunks = 64;
    std::vector<std::pair<std::int32_t, std::int32_t>> coords;
    coords.reserve(kChunks);
    for (int i = 0; i < kChunks; ++i) {
        coords.emplace_back(2000 + i, -2000);
    }

    const auto sequential_start = std::chrono::steady_clock::now();
    for (const auto& [cx, cz] : coords) {
        const auto chunk = generator.generate(cx, cz);
        ASSERT_EQ(chunk.x, cx);
    }
    const double sequential_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - sequential_start).count();

    auto& jobs = ChunkJobs::shared();
    const auto pooled_start = std::chrono::steady_clock::now();
    std::vector<std::future<PreparedChunk>> pending;
    pending.reserve(coords.size());
    for (const auto& [cx, cz] : coords) {
        pending.push_back(jobs.prepare(terila::world::DimensionKind::Overworld, cx, cz));
    }
    for (auto& job : pending) {
        ASSERT_FALSE(job.get().encoded.empty());
    }
    const double pooled_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - pooled_start).count();

    std::cout << "chunk jobs: " << jobs.threads() << " threads, " << kChunks
              << " chunks: " << sequential_ms << " ms sequential ("
              << sequential_ms / kChunks << " ms/chunk), " << pooled_ms << " ms pooled ("
              << pooled_ms / kChunks << " ms/chunk)" << std::endl;
}

namespace {

    std::uint64_t column_key(std::int32_t x, std::int32_t z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
            static_cast<std::uint32_t>(z);
    }

} // namespace

// What the light engine costs per chunk on its own: the columns come from real generated
// chunks and the tops are precomputed, so this measures the engine (fill, flood fill and
// packing) and not the terrain sampling it is handed in the server.
TEST(LightBench, ComputePerChunk) {
    REQUIRE_VANILLA_DATA();
    if (std::getenv("TERILA_BENCH") == nullptr) {
        GTEST_SKIP() << "set TERILA_BENCH=1 to run the light engine benchmark";
    }

    auto generator = make_generator(2026);

    std::unordered_map<std::uint64_t, Chunk> grid;
    for (std::int32_t cz = -1; cz <= 1; ++cz) {
        for (std::int32_t cx = -1; cx <= 1; ++cx) {
            grid.emplace(column_key(cx, cz), generator.generate(cx, cz));
        }
    }

    const auto& rules = generator.light_rules();
    const Chunk& center = grid.at(column_key(0, 0));
    const int world_top = center.min_y + center.section_count * 16 - 1;

    const auto state_in = [](const Chunk& chunk, int x, int y, int z) -> std::int32_t {
        const int local_y = y - chunk.min_y;
        if (local_y < 0 || local_y >= chunk.section_count * 16) {
            return 0;
        }
        const auto section = static_cast<std::size_t>(local_y / 16);
        return section_state(chunk.sections[section],
            ((local_y % 16) * 16 + (z & 15)) * 16 + (x & 15));
    };

    std::size_t sample_calls = 0;
    const auto sample = [&](int x, int y, int z) -> std::int32_t {
        ++sample_calls;
        const auto found = grid.find(column_key(x >> 4, z >> 4));
        return found == grid.end() ? 0 : state_in(found->second, x, y, z);
    };

    // The column tops of the whole halo, once: the server's generator caches these as well.
    std::unordered_map<std::uint64_t, int> tops;
    std::size_t top_calls = 0;
    const auto sky_top = [&](int x, int z) -> int {
        ++top_calls;
        const auto found = tops.find(column_key(x, z));
        if (found != tops.end()) {
            return found->second;
        }
        int top = center.min_y - 1;
        const auto chunk = grid.find(column_key(x >> 4, z >> 4));
        if (chunk != grid.end()) {
            for (int y = world_top; y >= chunk->second.min_y; --y) {
                if (!rules.of(state_in(chunk->second, x, y, z)).sky_down) {
                    top = y;
                    break;
                }
            }
        }
        tops.emplace(column_key(x, z), top);
        return top;
    };

    Chunk chunk = center;
    constexpr int kRuns = 20;
    const auto start = std::chrono::steady_clock::now();
    for (int run = 0; run < kRuns; ++run) {
        terila::world::LightEngine::compute(chunk, rules, sky_top, sample);
    }
    const double light_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count() / kRuns;

    // The same 16 chunk sweep with the halo cache off and on: the terrain, the column cache
    // and the machine are identical, so the difference is what the table is worth.
    constexpr int kChunks = 16;
    const auto sweep = [&](bool cache) -> double {
        terila::world::BlockSampleCache::set_enabled(cache);
        terila::world::BlockSampleCache::reset_stats();
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < kChunks; ++i) {
            const auto generated = generator.generate(5000 + i, 5000);
            EXPECT_EQ(generated.sections.size(), static_cast<std::size_t>(center.section_count));
        }
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count() / kChunks;
    };

    // The machine is shared, so the best of a few runs is the number worth printing.
    double without_cache_ms = 1e9;
    double with_cache_ms = 1e9;
    for (int run = 0; run < 3; ++run) {
        without_cache_ms = std::min(without_cache_ms, sweep(false));
        with_cache_ms = std::min(with_cache_ms, sweep(true));
    }
    terila::world::BlockSampleCache::set_enabled(true);
    const double generate_ms = with_cache_ms;

    // What those callbacks cost in the server: the generator's own column-cached lookups.
    volatile std::int32_t sink = 0;
    const auto top_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 2116; ++i) {
        sink += generator.sky_top((i % 46) - 15, (i / 46) - 15);
    }
    const double top_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - top_start).count();

    const auto state_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 12705; ++i) {
        sink += generator.state_at((i % 46) - 15, 60 + (i % 24), (i / 46) - 15);
    }
    const double state_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - state_start).count();

    std::cout << "light engine: " << light_ms << " ms/chunk (engine only), generate + light: "
              << without_cache_ms << " ms/chunk without the halo cache, " << generate_ms
              << " with it, per compute: " << top_calls / kRuns
              << " sky_top calls (" << top_ms / 2116 * 1000.0 << " us each, " << top_ms
              << " ms total), " << sample_calls / kRuns << " halo samples ("
              << state_ms / 12705 * 1000.0 << " us each, " << state_ms << " ms total";
    const auto sample_stats = terila::world::BlockSampleCache::stats();
    std::cout << ", halo block cache: " << sample_stats.hits << " hits, "
              << sample_stats.misses << " misses" << std::endl;
}


// The device path is a second implementation of the same tree, so the only thing worth
// asserting is that the two produce the same world.
TEST(TerrainGeneratorGpuTest, FlattensTheOverworldDensityField) {
    REQUIRE_VANILLA_DATA();
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
    const auto generator = make_generator(1337);

    const auto& plan = generator.density_plan();
    EXPECT_TRUE(plan.usable()) << plan.unsupported;
    EXPECT_GT(plan.nodes.size(), 100u);
    EXPECT_GT(plan.noises.size(), 10u);
    EXPECT_FALSE(plan.layers.empty());
    EXPECT_EQ(plan.permutations.size(), plan.layers.size() * 256);
    EXPECT_FALSE(generator.device_backed()) << "the cpu backend must not touch the device";
}

TEST(TerrainGeneratorGpuTest, DeviceFieldProducesTheSameChunkAsTheCpu) {
    REQUIRE_VANILLA_DATA();
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
    const auto cpu_generator = make_generator(1337);
    const Chunk cpu_chunk = cpu_generator.generate(7, -3);

    worldgen::gpu::set_backend(worldgen::gpu::Backend::Gpu);
    const bool available = worldgen::gpu::DensityDevice::shared().available();
    const std::string reason = worldgen::gpu::DensityDevice::shared().reason();
    if (!available) {
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
        GTEST_SKIP() << "no device can run the density kernel: " << reason;
    }

    const std::size_t batches_before = worldgen::gpu::DensityDevice::shared().batch_count();
    const auto device_generator = make_generator(1337);
    ASSERT_TRUE(device_generator.device_backed());
    const Chunk device_chunk = device_generator.generate(7, -3);
    const std::size_t batches_after = worldgen::gpu::DensityDevice::shared().batch_count();
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);

    EXPECT_GT(batches_after, batches_before) << "the chunk did not reach the device";

    ASSERT_EQ(cpu_chunk.sections.size(), device_chunk.sections.size());
    std::size_t mismatched = 0;
    std::size_t first_section = 0;
    std::size_t first_index = 0;
    for (std::size_t section = 0; section < cpu_chunk.sections.size(); ++section) {
        const auto& cpu_states = cpu_chunk.sections[section].block_states;
        const auto& device_states = device_chunk.sections[section].block_states;
        if (cpu_states.size() != device_states.size()) {
            ++mismatched;
            continue;
        }
        for (std::size_t index = 0; index < cpu_states.size(); ++index) {
            if (cpu_states[index] != device_states[index]) {
                if (mismatched == 0) {
                    first_section = section;
                    first_index = index;
                }
                ++mismatched;
            }
        }
    }
    EXPECT_EQ(mismatched, 0u) << "the first differing cell is section " << first_section
                              << " index " << first_index;
}

TEST(TerrainGeneratorGpuTest, DeviceFieldMatchesTheCpuOverSeveralBiomes) {
    REQUIRE_VANILLA_DATA();
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Gpu);
    const bool available = worldgen::gpu::DensityDevice::shared().available();
    const std::string reason = worldgen::gpu::DensityDevice::shared().reason();
    if (!available) {
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
        GTEST_SKIP() << "no device can run the density kernel: " << reason;
    }

    const std::pair<std::int32_t, std::int32_t> spots[] = { { 0, 0 }, { 96, -48 } };
    for (const auto& [cx, cz] : spots) {
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
        const Chunk from_cpu = make_generator(4242).generate(cx, cz);
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Gpu);
        const auto device_generator = make_generator(4242);
        ASSERT_TRUE(device_generator.device_backed());
        const Chunk from_device = device_generator.generate(cx, cz);

        ASSERT_EQ(from_cpu.sections.size(), from_device.sections.size());
        for (std::size_t section = 0; section < from_cpu.sections.size(); ++section) {
            EXPECT_EQ(from_cpu.sections[section].block_states,
                from_device.sections[section].block_states)
                << "chunk " << cx << ',' << cz << " section " << section;
        }
    }
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
}

TEST(TerrainGeneratorGpuTest, DeviceAnswersTheSameValuesAsTheCpu) {
    REQUIRE_VANILLA_DATA();
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Gpu);
    if (!worldgen::gpu::DensityDevice::shared().available()) {
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
        GTEST_SKIP() << worldgen::gpu::DensityDevice::shared().reason();
    }
    const auto generator = make_generator(1337);
    std::vector<worldgen::gpu::GridPoint> points;
    for (int iy = 0; iy < 12; ++iy) {
        for (int iz = 0; iz < 3; ++iz) {
            for (int ix = 0; ix < 3; ++ix) {
                points.push_back(worldgen::gpu::GridPoint{ static_cast<double>(ix * 4 + 0),
                    static_cast<double>(iy * 8 - 64), static_cast<double>(iz * 4) });
            }
        }
    }
    std::vector<double> values;
    ASSERT_TRUE(worldgen::gpu::DensityDevice::shared().evaluate(generator.density_plan(),
        points, values));
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);

    std::size_t differing = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double cpu = generator.density_at_point(points[i].x, points[i].y, points[i].z);
        if (cpu != values[i]) {
            ++differing;
        }
    }
    EXPECT_EQ(differing, 0u) << "the device and the chunk workers disagree on the field";
}

TEST(TerrainGeneratorGpuBench, BackendThroughput) {
    REQUIRE_VANILLA_DATA();
    if (std::getenv("TERILA_BENCH") == nullptr) {
        GTEST_SKIP() << "set TERILA_BENCH=1 to run the backend benchmark";
    }
    constexpr int kChunks = 16;
    std::vector<std::pair<std::int32_t, std::int32_t>> coords;
    for (int i = 0; i < kChunks; ++i) {
        coords.emplace_back(-400 + i * 3, 600 + i * 5);
    }

    const auto time_of = [&](worldgen::gpu::Backend backend, const char* label) {
        worldgen::gpu::set_backend(backend);
        const auto generator = make_generator(20260101);
        const auto start = std::chrono::steady_clock::now();
        for (const auto& [cx, cz] : coords) {
            generator.generate(cx, cz);
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << label << ": " << elapsed << " ms for " << kChunks << " chunks, "
                  << elapsed / kChunks << " ms/chunk" << std::endl;
        return elapsed;
    };

    const double cpu_ms = time_of(worldgen::gpu::Backend::Cpu, "cpu");
    if (!worldgen::gpu::DensityDevice::shared().available()) {
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
        GTEST_SKIP() << worldgen::gpu::DensityDevice::shared().reason();
    }
    const double device_ms = time_of(worldgen::gpu::Backend::Gpu, "gpu");
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
    std::cout << "gpu/cpu ratio: " << device_ms / cpu_ms << std::endl;
}

TEST(TerrainGeneratorGpuBench, ParallelThroughput) {
    REQUIRE_VANILLA_DATA();
    if (std::getenv("TERILA_BENCH") == nullptr) {
        GTEST_SKIP() << "set TERILA_BENCH=1 to run the backend benchmark";
    }
    constexpr int kChunks = 64;
    constexpr int kWorkers = 8;
    std::vector<std::pair<std::int32_t, std::int32_t>> coords;
    for (int i = 0; i < kChunks; ++i) {
        coords.emplace_back(-400 + i * 3, 600 + i * 5);
    }

    const auto parallel_time = [&](worldgen::gpu::Backend backend, const char* label) {
        worldgen::gpu::set_backend(backend);
        const auto generator = make_generator(20260101);
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::future<void>> work;
        std::atomic<std::size_t> next{ 0 };
        for (int worker = 0; worker < kWorkers; ++worker) {
            work.push_back(std::async(std::launch::async, [&] {
                for (;;) {
                    const std::size_t index = next.fetch_add(1);
                    if (index >= coords.size()) {
                        return;
                    }
                    generator.generate(coords[index].first, coords[index].second);
                }
            }));
        }
        for (auto& future : work) {
            future.get();
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << label << " x" << kWorkers << ": " << elapsed << " ms for " << kChunks
                  << " chunks, " << elapsed / kChunks << " ms/chunk" << std::endl;
        return elapsed;
    };

    parallel_time(worldgen::gpu::Backend::Cpu, "cpu");
    if (!worldgen::gpu::DensityDevice::shared().available()) {
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
        GTEST_SKIP() << worldgen::gpu::DensityDevice::shared().reason();
    }
    parallel_time(worldgen::gpu::Backend::Gpu, "gpu");
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
}

TEST(TerrainGeneratorGpuBench, FieldBatchCost) {
    REQUIRE_VANILLA_DATA();
    if (std::getenv("TERILA_BENCH") == nullptr) {
        GTEST_SKIP() << "set TERILA_BENCH=1 to run the backend benchmark";
    }
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Gpu);
    if (!worldgen::gpu::DensityDevice::shared().available()) {
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
        GTEST_SKIP() << worldgen::gpu::DensityDevice::shared().reason();
    }
    const auto generator = make_generator(20260101);
    constexpr int kChunks = 16;
    const auto points_of = [](int chunk) {
        std::vector<worldgen::gpu::GridPoint> out;
        out.reserve(48 * 5 * 5);
        const int wx0 = -400 + chunk * 16;
        const int wz0 = 600;
        for (int iy = 0; iy < 48; ++iy) {
            for (int iz = 0; iz < 5; ++iz) {
                for (int ix = 0; ix < 5; ++ix) {
                    out.push_back(worldgen::gpu::GridPoint{ static_cast<double>(wx0 + ix * 4),
                        static_cast<double>(-64 + iy * 8),
                        static_cast<double>(wz0 + iz * 4) });
                }
            }
        }
        return out;
    };

    std::vector<double> values;
    std::size_t points_per_chunk = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int chunk = 0; chunk < kChunks; ++chunk) {
        const auto points = points_of(chunk);
        points_per_chunk = points.size();
        ASSERT_TRUE(worldgen::gpu::DensityDevice::shared().evaluate(generator.density_plan(),
            points, values));
    }
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "device field: " << elapsed << " ms for " << kChunks << " chunks, "
              << elapsed / kChunks << " ms/chunk (" << points_per_chunk
              << " points/chunk)" << std::endl;

    // The same sixteen chunks in a single dispatch: the shape a batched device path would
    // have. If the per-chunk cost barely moves, the dispatch itself is not what costs, and
    // batching would buy nothing.
    std::vector<worldgen::gpu::GridPoint> merged;
    merged.reserve(points_per_chunk * kChunks);
    for (int chunk = 0; chunk < kChunks; ++chunk) {
        const auto points = points_of(chunk);
        merged.insert(merged.end(), points.begin(), points.end());
    }
    std::vector<double> merged_values;
    const auto merged_start = std::chrono::steady_clock::now();
    ASSERT_TRUE(worldgen::gpu::DensityDevice::shared().evaluate(generator.density_plan(),
        merged, merged_values));
    const double merged_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - merged_start).count();
    std::cout << "device field, one dispatch: " << merged_ms << " ms for " << kChunks
              << " chunks, " << merged_ms / kChunks << " ms/chunk" << std::endl;
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
}

TEST(TerrainGeneratorGpuTest, DeviceFieldMatchesTheCpuInEveryDimension) {
    REQUIRE_VANILLA_DATA();
    const DimensionKind kinds[] = { DimensionKind::Nether, DimensionKind::End };
    for (const DimensionKind kind : kinds) {
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
        terrain::Generator cpu_generator;
        if (!cpu_generator.init(-64, 16, 1337, mock_block, mock_biome, {}, kind)) {
            continue;
        }
        const Chunk from_cpu = cpu_generator.generate(3, -2);

        worldgen::gpu::set_backend(worldgen::gpu::Backend::Gpu);
        if (!worldgen::gpu::DensityDevice::shared().available()) {
            worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
            GTEST_SKIP() << worldgen::gpu::DensityDevice::shared().reason();
        }
        terrain::Generator device_generator;
        ASSERT_TRUE(device_generator.init(-64, 16, 1337, mock_block, mock_biome, {}, kind));
        const bool backed = device_generator.device_backed();
        const Chunk from_device = device_generator.generate(3, -2);
        worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);

        if (!backed) {
            continue;
        }
        ASSERT_EQ(from_cpu.sections.size(), from_device.sections.size());
        for (std::size_t section = 0; section < from_cpu.sections.size(); ++section) {
            EXPECT_EQ(from_cpu.sections[section].block_states,
                from_device.sections[section].block_states) << "section " << section;
        }
    }
}

TEST(TerrainGeneratorGpuTest, AutoBackendKeepsTheSameChunk) {
    REQUIRE_VANILLA_DATA();
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);
    const Chunk from_cpu = make_generator(777).generate(-5, 12);

    worldgen::gpu::set_backend(worldgen::gpu::Backend::Auto);
    const auto auto_generator = make_generator(777);
    const Chunk from_auto = auto_generator.generate(-5, 12);
    worldgen::gpu::set_backend(worldgen::gpu::Backend::Cpu);

    ASSERT_EQ(from_cpu.sections.size(), from_auto.sections.size());
    for (std::size_t section = 0; section < from_cpu.sections.size(); ++section) {
        EXPECT_EQ(from_cpu.sections[section].block_states, from_auto.sections[section].block_states)
            << "section " << section;
    }
}
