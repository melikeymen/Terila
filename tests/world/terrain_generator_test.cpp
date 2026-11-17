#include <gtest/gtest.h>
#include "world/terrain_generator.hpp"
#include "world/world_generator.hpp"
#include "world/chunk_encoder.hpp"
#include "world/chunk_decoder.hpp"
#include "world/block_world.hpp"

#include <cstdlib>
#include <optional>
#include <string_view>

using namespace terila::world;

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
    terrain::Generator generator;
    const auto missing = [](std::string_view name) -> std::optional<std::int32_t> {
        if (name == "minecraft:cactus") return std::nullopt;
        return mock_block(name);
        };
    EXPECT_FALSE(generator.init(-64, 24, 1337, missing, mock_biome));
}

TEST(TerrainGeneratorTest, FailsWhenABiomeIsUnknown) {
    terrain::Generator generator;
    const auto missing = [](std::string_view name) -> std::optional<std::int32_t> {
        if (name == "minecraft:taiga") return std::nullopt;
        return mock_biome(name);
        };
    EXPECT_FALSE(generator.init(-64, 24, 1337, mock_block, missing));
}

TEST(TerrainGeneratorTest, SameSeedGeneratesTheSameChunk) {
    const auto first = make_generator(1337);
    const auto second = make_generator(1337);
    EXPECT_EQ(encode::encode(first.generate(3, -2)), encode::encode(second.generate(3, -2)));
}

TEST(TerrainGeneratorTest, DifferentSeedsGenerateDifferentTerrain) {
    const auto first = make_generator(1337);
    const auto second = make_generator(7331);
    EXPECT_NE(encode::encode(first.generate(0, 0)), encode::encode(second.generate(0, 0)));
}

TEST(TerrainGeneratorTest, ColumnHasBedrockUnderSurfaceAndAirAbove) {
    const auto generator = make_generator();
    const auto chunk = generator.generate(0, 0);
    EXPECT_EQ(section_block(chunk, 8, generator.min_y(), 8), 7);   // bedrock

    const int surface = generator.surface_height(8, 8);
    EXPECT_EQ(section_block(chunk, 8, surface, 8), generator.state_at(8, surface, 8));
    EXPECT_EQ(section_block(chunk, 8, generator.sea_level() + 60, 8), 0);  // air high up
}

TEST(TerrainGeneratorTest, UnderwaterColumnsHoldWaterAtSeaLevel) {
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
                    if (surface == 42 || (!grass_only && height > generator.sea_level())) {
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
    const auto generator = make_generator();
    const LandColumn land = find_column(generator, true);
    ASSERT_NE(land.biome, -1) << "no grassy column within 4096 blocks";

    int plants = 0;
    const auto chunk = generator.generate(land.x / 16, land.z / 16);
    for (const auto& section : chunk.sections) {
        for (int i = 0; i < 4096; ++i) {
            if (is_plant(section_state(section, i))) {
                ++plants;
            }
        }
    }
    EXPECT_GT(plants, 0);
}

TEST(TerrainGeneratorTest, CarvesCavesUnderTheSurface) {
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

TEST(TerrainGeneratorTest, CoversManyBiomesAcrossTheMap) {
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


