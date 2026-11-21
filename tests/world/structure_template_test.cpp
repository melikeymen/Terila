#include "world/worldgen/structures/placement.hpp"
#include "world/worldgen/structures/structures.hpp"
#include "world/worldgen/structures/template.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

    using terila::data::BlockStateInfo;
    using terila::data::RegistryData;

    // The registry a test needs: one block with two variants and one block with a single
    // state, which is the shape blocks/state_ids.json has.
    RegistryData small_registry() {
        RegistryData registry;
        BlockStateInfo bottom;
        bottom.id = 100;
        bottom.properties = { { "facing", "north" }, { "half", "bottom" } };
        BlockStateInfo top;
        top.id = 101;
        top.properties = { { "facing", "north" }, { "half", "top" } };
        registry.block_states["minecraft:stone_brick_stairs"] = { bottom, top };
        registry.block_state_ids["minecraft:stone_brick_stairs"] = 100;
        registry.block_state_ids["minecraft:stone"] = 1;
        return registry;
    }

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

    TEST(StructureTemplateTest, ResolvesAStateByNameAndProperties) {
        const RegistryData registry = small_registry();
        EXPECT_EQ(terila::world::worldgen::template_state_id(
                      "minecraft:stone_brick_stairs[facing=north,half=top]",
                      registry.block_states, registry.block_state_ids), 101);
        EXPECT_EQ(terila::world::worldgen::template_state_id("minecraft:stone",
                      registry.block_states, registry.block_state_ids), 1);
    }

    TEST(StructureTemplateTest, FallsBackToTheDefaultStateOfAKnownBlock) {
        const RegistryData registry = small_registry();
        EXPECT_EQ(terila::world::worldgen::template_state_id(
                      "minecraft:stone_brick_stairs[half=sideways]",
                      registry.block_states, registry.block_state_ids), 100);
        EXPECT_EQ(terila::world::worldgen::template_state_id("minecraft:not_a_block",
                      registry.block_states, registry.block_state_ids), 0);
    }

    TEST(StructureTemplateTest, LoadsATemplateFromTheVanillaDump) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        // The extractor writes one file per template the game ships, so the loader is read
        // against a real one instead of a fixture. This portal is six by ten by six.
        // Only the two tables the state lookup reads are needed here, so the dump is read
        // straight instead of loading the whole registry.
        terila::data::RegistryData registry;
        {
            std::string text;
            ASSERT_TRUE(terila::data::input::read_text("blocks/state_ids.json", text));
            const nlohmann::json dump = nlohmann::json::parse(text);
            for (const nlohmann::json& entry : dump.at("data")) {
                const std::string block = entry.at("id").get<std::string>();
                registry.block_state_ids[block] = entry.at("default_state_id").get<std::int32_t>();
                for (const nlohmann::json& variant : entry.at("states")) {
                    terila::data::BlockStateInfo info;
                    info.id = variant.at("id").get<std::int32_t>();
                    const nlohmann::json properties =
                        variant.value("properties", nlohmann::json::object());
                    for (auto property = properties.begin(); property != properties.end(); ++property) {
                        info.properties[property.key()] = property.value().get<std::string>();
                    }
                    registry.block_states[block].push_back(info);
                }
            }
        }
        ASSERT_FALSE(registry.block_states.empty());

        terila::world::worldgen::StructureTemplate portal;
        ASSERT_TRUE(terila::world::worldgen::load_template(
            "minecraft:ruined_portal/portal_1", registry, portal));
        EXPECT_EQ(portal.size_x, 6);
        EXPECT_EQ(portal.size_y, 10);
        EXPECT_EQ(portal.size_z, 6);
        ASSERT_FALSE(portal.empty());

        int solid = 0;
        int air = 0;
        for (const auto& cell : portal.cells) {
            if (cell.state == 0) {
                ++air;
                continue;
            }
            ASSERT_LT(cell.x, portal.size_x);
            ASSERT_LT(cell.y, portal.size_y);
            ASSERT_LT(cell.z, portal.size_z);
            ++solid;
        }
        // The portal is mostly the air inside it, so the check is that both sides are read.
        EXPECT_GT(solid, 50);
        EXPECT_GT(air, 0);
    }


    TEST(StructurePlacementTest, SitesStayInsideTheirCellAndRepeat) {
        using terila::world::worldgen::StructureSite;
        terila::world::worldgen::StructureSpread spread;
        spread.salt = 34222645;
        spread.spacing = 40;
        spread.separation = 15;

        // The same seed, cell and salt must always give the same chunk: a world is rebuilt
        // from its seed and its structures may not move.
        for (std::int32_t gz = -2; gz <= 2; ++gz) {
            for (std::int32_t gx = -2; gx <= 2; ++gx) {
                const StructureSite first =
                    terila::world::worldgen::spread_site(12345, spread, gx, gz);
                const StructureSite again =
                    terila::world::worldgen::spread_site(12345, spread, gx, gz);
                ASSERT_TRUE(first.valid);
                EXPECT_EQ(first.chunk_x, again.chunk_x);
                EXPECT_EQ(first.chunk_z, again.chunk_z);

                // A site keeps the separation to the cell edge, so two neighbouring sites
                // cannot end up on top of each other.
                const int low = spread.spacing - spread.separation;
                EXPECT_GE(first.chunk_x, gx * spread.spacing + spread.separation / 2);
                EXPECT_LT(first.chunk_x, gx * spread.spacing + spread.separation / 2 + low + 1);
                EXPECT_GE(first.chunk_z, gz * spread.spacing + spread.separation / 2);
                EXPECT_LT(first.chunk_z, gz * spread.spacing + spread.separation / 2 + low + 1);
            }
        }

        const StructureSite centre = terila::world::worldgen::spread_site(12345, spread, 0, 0);
        EXPECT_EQ(terila::world::worldgen::spread_cell(centre.chunk_x, spread.spacing), 0);
        EXPECT_EQ(terila::world::worldgen::spread_cell(centre.chunk_z, spread.spacing), 0);

        // Another salt is another arrangement of the same grid.
        terila::world::worldgen::StructureSpread other = spread;
        other.salt += 1;
        bool moved = false;
        for (std::int32_t gz = -2; gz <= 2 && !moved; ++gz) {
            for (std::int32_t gx = -2; gx <= 2 && !moved; ++gx) {
                const StructureSite a = terila::world::worldgen::spread_site(12345, spread, gx, gz);
                const StructureSite b = terila::world::worldgen::spread_site(12345, other, gx, gz);
                moved = a.chunk_x != b.chunk_x || a.chunk_z != b.chunk_z;
            }
        }
        EXPECT_TRUE(moved);
    }

    TEST(StructurePlacementTest, ATemplateIsWrittenIntoTheChunkItsCellsLandIn) {
        terila::world::worldgen::StructureTemplate tmpl;
        tmpl.size_x = 17;
        tmpl.size_y = 2;
        tmpl.size_z = 2;
        tmpl.cells = {
            { 0, 0, 0, 11 },
            { 15, 0, 0, 12 },
            { 16, 0, 0, 13 },
            { 3, 1, 0, 0 },
        };

        int written = 0;
        int wrong_chunk = 0;
        const int placed = terila::world::worldgen::apply_template(tmpl, 0, 64, 0, 0, 0,
            [&](std::int32_t wx, std::int32_t wy, std::int32_t wz, std::int32_t state) {
                ++written;
                EXPECT_GE(wy, 64);
                EXPECT_NE(state, 0);
                if ((wx >> 4) != 0 || (wz >> 4) != 0) {
                    ++wrong_chunk;
                }
            });
        EXPECT_EQ(placed, 2);
        EXPECT_EQ(written, 2);
        EXPECT_EQ(wrong_chunk, 0);

        // The same template writes its other column when the neighbour chunk asks for it.
        int neighbour = 0;
        terila::world::worldgen::apply_template(tmpl, 0, 64, 0, 1, 0,
            [&](std::int32_t, std::int32_t, std::int32_t, std::int32_t) { ++neighbour; });
        EXPECT_EQ(neighbour, 1);
    }


    TEST(StructurePlacementTest, AStructureOnlyGrowsInItsBiomes) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        terila::world::worldgen::Structures structures;
        const auto any_block = [](std::string_view) -> std::optional<std::int32_t> { return 5; };
        const auto every_biome = [](std::string_view) { return 100; };
        ASSERT_TRUE(structures.init(1337, any_block, every_biome,
            terila::world::worldgen::built_structures()));

        terila::world::worldgen::StructureSpread spread;
        spread.salt = 34222645;
        spread.spacing = 40;
        spread.separation = 15;
        const auto site = terila::world::worldgen::spread_site(1337, spread, 0, 0);
        ASSERT_TRUE(site.valid);

        const auto surface = [](std::int32_t, std::int32_t) { return 70; };
        int allowed = 0;
        structures.apply(site.chunk_x, site.chunk_z, 63, surface,
            [](std::int32_t, std::int32_t) { return 100; },
            [&allowed](std::int32_t, std::int32_t, std::int32_t, std::int32_t) { ++allowed; });
        EXPECT_GT(allowed, 0);

        // The same site in a biome the structure's tag does not name stays empty.
        int rejected = 0;
        structures.apply(site.chunk_x, site.chunk_z, 63, surface,
            [](std::int32_t, std::int32_t) { return 101; },
            [&rejected](std::int32_t, std::int32_t, std::int32_t, std::int32_t) { ++rejected; });
        EXPECT_EQ(rejected, 0);
    }


    TEST(StructurePlacementTest, AWreckOnlyLiesUnderTheWater) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        terila::world::worldgen::Structures structures;
        const auto any_block = [](std::string_view) -> std::optional<std::int32_t> { return 5; };
        const auto every_biome = [](std::string_view) { return 100; };
        ASSERT_TRUE(structures.init(1337, any_block, every_biome,
            terila::world::worldgen::built_structures()));

        // The shipwreck set has its own salt; a hull belongs on the sea floor, not on a hill.
        terila::world::worldgen::StructureSpread spread;
        spread.salt = 165745295;
        spread.spacing = 24;
        spread.separation = 4;
        const auto site = terila::world::worldgen::spread_site(1337, spread, 0, 0);
        ASSERT_TRUE(site.valid);

        // Only the wet hull is asked for here: the beached one is allowed on dry land.
        std::vector<terila::world::worldgen::StructureKind> wet;
        for (auto kind : terila::world::worldgen::built_structures()) {
            if (kind.id == "minecraft:shipwreck") {
                wet.push_back(std::move(kind));
            }
        }
        terila::world::worldgen::Structures wreck_only;
        ASSERT_TRUE(wreck_only.init(1337, any_block, every_biome, wet));

        const auto count_at = [&](std::int32_t floor_y) {
            int placed = 0;
            wreck_only.apply(site.chunk_x, site.chunk_z, 63,
                [floor_y](std::int32_t, std::int32_t) { return floor_y; },
                [](std::int32_t, std::int32_t) { return 100; },
                [&placed](std::int32_t, std::int32_t, std::int32_t, std::int32_t) { ++placed; });
            return placed;
        };
        EXPECT_GT(count_at(40), 0);
        EXPECT_EQ(count_at(70), 0);
    }

    TEST(StructurePlacementTest, AStructureUsesTheStateThePaletteNames) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        terila::world::worldgen::Structures structures;
        const auto any_block = [](std::string_view) -> std::optional<std::int32_t> { return 5; };
        const auto every_biome = [](std::string_view) { return 100; };
        // The variant table behind the palette decides what lands: when the caller can name a
        // full state, every template cell must carry it instead of the block's default.
        const terila::world::worldgen::StateTextResolver palette =
            [](std::string_view) { return 4242; };
        ASSERT_TRUE(structures.init(1337, any_block, every_biome,
            terila::world::worldgen::built_structures(), palette));

        terila::world::worldgen::StructureSpread spread;
        spread.salt = 34222645;
        spread.spacing = 40;
        spread.separation = 15;
        const auto site = terila::world::worldgen::spread_site(1337, spread, 0, 0);

        int from_palette = 0;
        int from_block_default = 0;
        structures.apply(site.chunk_x, site.chunk_z, 63,
            [](std::int32_t, std::int32_t) { return 70; },
            [](std::int32_t, std::int32_t) { return 100; },
            [&](std::int32_t, std::int32_t, std::int32_t, std::int32_t state) {
                (state == 4242 ? from_palette : from_block_default) += 1;
            });
        EXPECT_GT(from_palette, 0);
        EXPECT_EQ(from_block_default, 0);
    }


    TEST(JigsawTest, PillagerOutpostAssemblesFromPools) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        using terila::world::worldgen::assemble_jigsaw;
        using terila::world::worldgen::load_pool;

        // The pillager outpost start pool should load and contain elements.
        auto pool_opt = load_pool("minecraft:pillager_outpost/base_plates");
        ASSERT_TRUE(pool_opt.has_value());
        EXPECT_FALSE(pool_opt->elements.empty());
        EXPECT_EQ(pool_opt->id, "minecraft:pillager_outpost/base_plates");

        // Assemble a small outpost at a fixed position.
        const auto resolve = [](std::string_view) { return 0; };
        auto pieces = assemble_jigsaw("minecraft:pillager_outpost/base_plates", resolve,
            0, 70, 0, 12345);
        // Should place at least the base plate and some connected pieces.
        EXPECT_GT(pieces.size(), 0);
        // The base plate should be centered at the origin.
        EXPECT_EQ(pieces[0].origin_x, 0);
        EXPECT_EQ(pieces[0].origin_z, 0);
    }

    TEST(JigsawTest, VillageTownCenterAssemblesFromPools) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        using terila::world::worldgen::assemble_jigsaw;
        using terila::world::worldgen::load_pool;

        // The plains village town centers pool should load.
        auto pool_opt = load_pool("minecraft:village/plains/town_centers");
        ASSERT_TRUE(pool_opt.has_value());
        EXPECT_FALSE(pool_opt->elements.empty());

        const auto resolve = [](std::string_view) { return 0; };
        auto pieces = assemble_jigsaw("minecraft:village/plains/town_centers", resolve,
            0, 70, 0, 1337, 12);
        EXPECT_GT(pieces.size(), 0);
    }

    TEST(JigsawTest, AncientCityAssemblesFromPools) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        using terila::world::worldgen::assemble_jigsaw;
        using terila::world::worldgen::load_pool;

        // The ancient city center pool should load.
        auto pool_opt = load_pool("minecraft:ancient_city/city_center");
        ASSERT_TRUE(pool_opt.has_value());
        EXPECT_FALSE(pool_opt->elements.empty());

        const auto resolve = [](std::string_view) { return 0; };
        auto pieces = assemble_jigsaw("minecraft:ancient_city/city_center", resolve,
            0, -27, 0, 1337, 8);
        EXPECT_GT(pieces.size(), 0);
    }

} // namespace
