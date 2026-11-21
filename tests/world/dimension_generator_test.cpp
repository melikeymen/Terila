#include "data/input_store.hpp"
#include "world/block_world.hpp"
#include "world/world_context.hpp"
#include "world/world_generator.hpp"
#include "world/worldgen/dimension.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

    using terila::world::Chunk;
    using terila::world::worldgen::DimensionKind;
    using terila::world::worldgen::DimensionType;

    bool input_available() {
        static const bool available = [] {
            std::error_code ec;
            for (const char* prefix : { "input/", "", "../input/", "../../input/", "../../../input/" }) {
                const std::filesystem::path root(prefix);
                if (std::filesystem::is_regular_file(root / "blocks" / "state_ids.json", ec) && !ec) {
                    terila::data::input::mount(root.string());
                    return true;
                }
                ec.clear();
            }
            return false;
        }();
        return available;
    }

    // Deliberately synthetic ids: every name maps to a fixed state number, so the test never
    // depends on the order a registry dump happens to be in. Air stays zero.
    std::int32_t id_of(std::string_view name) {
        if (name == "minecraft:air") {
            return 0;
        }
        std::uint64_t hash = 1469598103934665603ull;
        for (const char character : name) {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ull;
        }
        return static_cast<std::int32_t>(1 + hash % 60000);
    }

    std::optional<std::int32_t> block_id(std::string_view name) { return id_of(name); }
    std::optional<std::int32_t> biome_id(std::string_view name) { return id_of(name); }

    bool init_dimension(terila::world::WorldGenerator& generator, DimensionKind kind) {
        const DimensionType type = terila::world::worldgen::dimension_type_of(kind);
        return generator.init(terila::world::WorldType::Terrain, type.min_y,
            type.height / 16, 12345, block_id, biome_id, {}, kind);
    }

    template<class Fn>
    void for_each_state(const Chunk& chunk, Fn&& visit) {
        for (std::size_t section = 0; section < chunk.sections.size(); ++section) {
            const std::int32_t section_y = chunk.min_y + static_cast<int>(section) * 16;
            for (int index = 0; index < 4096; ++index) {
                visit(section_y + index / 256, terila::world::section_state(
                    chunk.sections[section], index));
            }
        }
    }

} // namespace

TEST(DimensionTypeTest, ReadsTheShippedDimensions) {
    if (!input_available()) {
        GTEST_SKIP() << "input/ is not next to the build tree";
    }
    const DimensionType overworld = terila::world::worldgen::dimension_type_of(
        DimensionKind::Overworld);
    const DimensionType nether = terila::world::worldgen::dimension_type_of(
        DimensionKind::Nether);
    const DimensionType end = terila::world::worldgen::dimension_type_of(
        DimensionKind::End);

    EXPECT_EQ(overworld.min_y, -64);
    EXPECT_EQ(overworld.height, 384);
    EXPECT_TRUE(overworld.has_skylight);

    EXPECT_EQ(nether.min_y, 0);
    EXPECT_EQ(nether.height, 256);
    EXPECT_FALSE(nether.has_skylight);
    EXPECT_TRUE(nether.has_ceiling);
    EXPECT_EQ(nether.coordinate_scale, 8.0);

    EXPECT_EQ(end.min_y, 0);
    EXPECT_EQ(end.height, 256);
    EXPECT_TRUE(end.dragon_fight);
}

TEST(DimensionTypeTest, NamesTheTypeAndTheDimensionACommandCanReach) {
    using terila::world::worldgen::dimension_kind_of;
    using terila::world::worldgen::dimension_type_id;

    EXPECT_EQ(dimension_type_id(DimensionKind::Overworld), 0);
    EXPECT_EQ(dimension_type_id(DimensionKind::Nether), 2);
    EXPECT_EQ(dimension_type_id(DimensionKind::End), 3);

    DimensionKind kind = DimensionKind::Overworld;
    ASSERT_TRUE(dimension_kind_of("minecraft:the_nether", kind));
    EXPECT_EQ(kind, DimensionKind::Nether);
    ASSERT_TRUE(dimension_kind_of("minecraft:the_end", kind));
    EXPECT_EQ(kind, DimensionKind::End);
    EXPECT_FALSE(dimension_kind_of("minecraft:moon", kind));
}

TEST(BlockWorldTest, KeepsEachDimensionOnItsOwnCoordinate) {
    terila::world::BlockWorld world;
    const terila::world::BlockPos pos{ 4, 70, 4 };
    world.set(pos, 11);

    EXPECT_EQ(world.get(pos, 0), 11);
    EXPECT_EQ(world.get(pos, 0, DimensionKind::Nether), 0)
        << "the nether column picked up an overworld edit";

    world.set(pos, 22, DimensionKind::Nether);
    EXPECT_EQ(world.get(pos, 0), 11);
    EXPECT_EQ(world.get(pos, 0, DimensionKind::Nether), 22);
    EXPECT_EQ(world.get(pos, 0, DimensionKind::End), 0);

    EXPECT_TRUE(world.chunk_modified(0, 0));
    EXPECT_FALSE(world.chunk_modified(0, 0, DimensionKind::End));
}

TEST(WorldContextTest, HoldsADifferentGeneratorPerDimension) {
    auto& context = terila::world::WorldContext::shared();
    EXPECT_NE(&context.generator(DimensionKind::Overworld),
        &context.generator(DimensionKind::Nether));
    EXPECT_NE(&context.generator(DimensionKind::Nether),
        &context.generator(DimensionKind::End));
    EXPECT_NE(&context.chunks(DimensionKind::Overworld),
        &context.chunks(DimensionKind::Nether));
}

TEST(NetherGeneratorTest, FillsItsSeaWithLavaUnderANetherrackCeiling) {
    if (!input_available()) {
        GTEST_SKIP() << "input/ is not next to the build tree";
    }
    terila::world::WorldGenerator generator;
    ASSERT_TRUE(init_dimension(generator, DimensionKind::Nether));

    const Chunk chunk = generator.generate(0, 0);
    EXPECT_EQ(chunk.min_y, 0);
    EXPECT_EQ(chunk.section_count, 16);

    const std::int32_t netherrack = id_of("minecraft:netherrack");
    const std::int32_t lava = id_of("minecraft:lava");
    const std::int32_t water = id_of("minecraft:water");

    int netherrack_count = 0;
    int lava_count = 0;
    int water_count = 0;
    int lava_above_sea = 0;
    for_each_state(chunk, [&](int y, std::int32_t state) {
        if (state == netherrack) {
            ++netherrack_count;
        } else if (state == lava) {
            ++lava_count;
            if (y >= 32) {
                ++lava_above_sea;
            }
        } else if (state == water) {
            ++water_count;
        }
    });

    EXPECT_GT(netherrack_count, 1000) << "the nether column has no netherrack";
    EXPECT_GT(lava_count, 0) << "the lava sea below y=32 is missing";
    EXPECT_EQ(lava_above_sea, 0) << "lava above the sea level is not vanilla";
    EXPECT_EQ(water_count, 0) << "water does not belong in the nether";
}

TEST(EndGeneratorTest, BuildsTheCentralIslandOutOfEndStone) {
    if (!input_available()) {
        GTEST_SKIP() << "input/ is not next to the build tree";
    }
    terila::world::WorldGenerator generator;
    ASSERT_TRUE(init_dimension(generator, DimensionKind::End));

    const Chunk chunk = generator.generate(0, 0);
    EXPECT_EQ(chunk.min_y, 0);
    EXPECT_EQ(chunk.section_count, 16);

    const std::int32_t end_stone = id_of("minecraft:end_stone");
    const std::int32_t water = id_of("minecraft:water");

    int end_stone_count = 0;
    int water_count = 0;
    for_each_state(chunk, [&](int, std::int32_t state) {
        if (state == end_stone) {
            ++end_stone_count;
        } else if (state == water) {
            ++water_count;
        }
    });

    EXPECT_GT(end_stone_count, 1000) << "the central island has no end stone";
    EXPECT_EQ(water_count, 0) << "the end has no water sea";
}
