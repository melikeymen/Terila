#include "world/worldgen/structures/placement.hpp"
#include "world/worldgen/structures/structures.hpp"
#include "world/worldgen/structures/template.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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
        const int placed = terila::world::worldgen::apply_template(tmpl, 0, 64, 0, 0, 0, 0, {},
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
        terila::world::worldgen::apply_template(tmpl, 0, 64, 0, 1, 0, 0, {},
            [&](std::int32_t, std::int32_t, std::int32_t, std::int32_t) { ++neighbour; });
        EXPECT_EQ(neighbour, 1);
    }


    TEST(StructurePlacementTest, ATurnedTemplateWritesItsCellsAndStatesTurned) {
        using terila::world::worldgen::StructureTemplate;
        using terila::world::worldgen::apply_template;

        // A row of blocks three long and one deep: a quarter turn clockwise lays it along z.
        StructureTemplate tmpl;
        tmpl.size_x = 3;
        tmpl.size_y = 1;
        tmpl.size_z = 1;
        tmpl.cells.push_back({ 0, 0, 0, 5 });
        tmpl.cells.push_back({ 2, 0, 0, 5 });

        std::vector<std::pair<std::int32_t, std::int32_t>> columns;
        int turned_states = 0;
        const int placed = apply_template(tmpl, 0, 64, 0, 0, 0, 1,
            [](std::int32_t state, int quarter_turns) { return state + quarter_turns; },
            [&](std::int32_t x, std::int32_t y, std::int32_t z, std::int32_t state) {
                columns.emplace_back(x, z);
                if (state == 6) ++turned_states;
                EXPECT_EQ(y, 64);
            });

        EXPECT_EQ(placed, 2);
        ASSERT_EQ(columns.size(), 2u);
        EXPECT_EQ(columns[0].first, 0);
        EXPECT_EQ(columns[0].second, 0);
        EXPECT_EQ(columns[1].first, 0);
        EXPECT_EQ(columns[1].second, 2);
        EXPECT_EQ(turned_states, 2);
    }

    TEST(StructurePlacementTest, AGapUnderASingleTemplateIsFilledOnASlope) {
        using terila::world::worldgen::StructureTemplate;
        using terila::world::worldgen::fill_beard;

        // A four by four floor one layer above the template's bottom: the bottom cell of a
        // template is not always its layer 0, so the fill has to find the lowest solid one.
        StructureTemplate tmpl;
        tmpl.size_x = 4;
        tmpl.size_y = 2;
        tmpl.size_z = 4;
        for (std::int32_t x = 0; x < 4; ++x) {
            for (std::int32_t z = 0; z < 4; ++z) {
                tmpl.cells.push_back({ x, 1, z, 7 });
            }
        }

        const auto slope = [](std::int32_t x, std::int32_t) {
            return x < 2 ? 70 : 74;
        };
        const std::int32_t filler = 9;
        int filled = 0;
        std::int32_t highest = 0;
        std::int32_t lowest = 1000;
        fill_beard(tmpl, 0, 78, 0, 0, 0, 0, slope, filler,
            [&](std::int32_t wx, std::int32_t wy, std::int32_t, std::int32_t state) {
                EXPECT_EQ(state, filler);
                EXPECT_LT(wy, 79) << "column " << wx << " filled above the floor";
                ++filled;
                if (wy > highest) highest = wy;
                if (wy < lowest) lowest = wy;
            });

        // The floor sits at y 79; the western columns rise eight blocks to it and the
        // eastern ones four, so the structure stands on filled ground instead of hanging.
        EXPECT_EQ(filled, 8 * 8 + 8 * 4);
        EXPECT_EQ(highest, 78);
        EXPECT_EQ(lowest, 71);

        // Flat ground leaves no gap to fill.
        int on_flat = 0;
        fill_beard(tmpl, 0, 78, 0, 0, 0, 0,
            [](std::int32_t, std::int32_t) { return 78; }, filler,
            [&on_flat](std::int32_t, std::int32_t, std::int32_t, std::int32_t) { ++on_flat; });
        EXPECT_EQ(on_flat, 0);
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
        // A rotation table is present as it is on the server, so the site's turn is applied
        // to the cells and to every state.
        ASSERT_TRUE(structures.init(1337, any_block, every_biome,
            terila::world::worldgen::built_structures(), palette,
            [](std::int32_t state, int) { return state; }));

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

        // The houses and streets follow the ground they are placed on; without that flag the
        // assembly would put every piece at the height of the centre and the village would
        // float over a slope.
        int terrain_matching = 0;
        for (const auto& piece : pieces) {
            terrain_matching += piece.terrain_matching ? 1 : 0;
        }
        EXPECT_GT(terrain_matching, 0);
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

    TEST(JigsawTest, ExpansionStopsAtTheStructureDepth) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        using terila::world::worldgen::assemble_jigsaw;

        const auto resolve = [](std::string_view) { return 0; };
        const auto assemble = [&](int depth) {
            return assemble_jigsaw("minecraft:village/plains/town_centers", resolve,
                0, 70, 0, 1337, 64, depth, 80);
        };
        // The data bounds how many layers a village may grow; one layer is the town centre
        // and the pieces joined straight to it, six is the whole village.
        const auto shallow = assemble(1);
        const auto deep = assemble(6);
        ASSERT_GT(shallow.size(), 0u);
        EXPECT_GT(deep.size(), shallow.size());
    }

    TEST(JigsawTest, PiecesStayWithinTheStructuresDistanceFromCenter) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        using terila::world::worldgen::assemble_jigsaw;

        const auto resolve = [](std::string_view) { return 0; };
        const int max_distance = 24;
        const auto pieces = assemble_jigsaw("minecraft:village/plains/town_centers", resolve,
            0, 70, 0, 1337, 64, 6, max_distance);
        ASSERT_GT(pieces.size(), 0u);
        for (const auto& piece : pieces) {
            const std::int64_t dx = piece.origin_x + piece.tmpl.size_x / 2;
            const std::int64_t dz = piece.origin_z + piece.tmpl.size_z / 2;
            EXPECT_LE(dx * dx + dz * dz,
                static_cast<std::int64_t>(max_distance) * max_distance)
                << "a piece landed at " << dx << "," << dz;
        }
    }

    TEST(StructureTemplateTest, AQuarterTurnTurnsDirectionsAxesAndStairShapes) {
        using terila::world::worldgen::template_rotator;

        RegistryData registry;
        const auto add = [&registry](const std::string& block,
            std::initializer_list<std::pair<std::string, std::string>> properties,
            std::int32_t id) {
            BlockStateInfo info;
            info.id = id;
            info.properties = { properties.begin(), properties.end() };
            registry.block_states[block].push_back(info);
        };
        add("minecraft:oak_stairs", { { "facing", "north" }, { "half", "bottom" },
            { "shape", "straight" } }, 10);
        add("minecraft:oak_stairs", { { "facing", "east" }, { "half", "bottom" },
            { "shape", "straight" } }, 11);
        add("minecraft:oak_stairs", { { "facing", "north" }, { "half", "bottom" },
            { "shape", "inner_left" } }, 12);
        add("minecraft:oak_stairs", { { "facing", "east" }, { "half", "bottom" },
            { "shape", "inner_right" } }, 13);
        add("minecraft:oak_pillar", { { "axis", "y" } }, 20);
        add("minecraft:oak_pillar", { { "axis", "x" } }, 21);
        add("minecraft:oak_pillar", { { "axis", "z" } }, 22);

        const auto rotate = template_rotator(registry);
        EXPECT_EQ(rotate(10, 1), 11);   // north becomes east
        EXPECT_EQ(rotate(11, 3), 10);   // and three turns bring it back
        EXPECT_EQ(rotate(10, 2), 10);   // no south variant, so it is left as it is
        EXPECT_EQ(rotate(12, 1), 13);   // the stair's inner corner swaps sides
        EXPECT_EQ(rotate(21, 1), 22);   // the x and z axes exchange
        EXPECT_EQ(rotate(20, 1), 20);   // the y axis is the turn's axis
        EXPECT_EQ(rotate(10, 0), 10);   // no turn, no change
    }

    TEST(JigsawTest, JigsawOrientationsAreReadFromTheTemplateState) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        using terila::world::worldgen::JigsawOrientation;
        using terila::world::worldgen::StructureTemplate;
        using terila::world::worldgen::JigsawBlock;
        using terila::world::worldgen::load_template_with_jigsaws;
        using terila::world::worldgen::parse_orientation;

        StructureTemplate tmpl;
        std::vector<JigsawBlock> jigsaws;
        ASSERT_TRUE(load_template_with_jigsaws("minecraft:village/plains/streets/corner_01",
            [](std::string_view) { return 0; }, tmpl, jigsaws));
        ASSERT_FALSE(jigsaws.empty());
        int known = 0;
        for (const auto& jigsaw : jigsaws) {
            if (parse_orientation(jigsaw.orientation) != JigsawOrientation::Unknown) {
                ++known;
            }
        }
        // The state text names the orientation, so every jigsaw of a real template is known.
        EXPECT_EQ(known, static_cast<int>(jigsaws.size()));
    }

    TEST(JigsawTest, ARotatedPieceTurnsItsCellsAndItsStates) {
        using terila::world::worldgen::PlacedPiece;
        using terila::world::worldgen::write_jigsaw_pieces;

        // A three by one row of blocks, laid down with one quarter turn clockwise: a cell at
        // x becomes a cell at z.
        PlacedPiece piece;
        piece.tmpl.size_x = 3;
        piece.tmpl.size_y = 1;
        piece.tmpl.size_z = 1;
        piece.origin_y = 64;
        piece.rotation = 1;
        piece.tmpl.cells.push_back({ 0, 0, 0, 5 });
        piece.tmpl.cells.push_back({ 1, 0, 0, 5 });
        piece.tmpl.cells.push_back({ 2, 0, 0, 5 });

        std::vector<std::pair<std::int32_t, std::int32_t>> columns;
        int turned_states = 0;
        write_jigsaw_pieces(std::vector<PlacedPiece>{ piece }, 0, 0,
            [](std::int32_t, std::int32_t) { return 63; }, 9,
            [](std::int32_t, int quarter_turns) { return 55 + quarter_turns; },
            [&](std::int32_t x, std::int32_t y, std::int32_t z, std::int32_t state) {
                if (state == 9) return;
                columns.emplace_back(x, z);
                if (state == 56) ++turned_states;
                EXPECT_EQ(y, 64);
            });

        ASSERT_EQ(columns.size(), 3u);
        EXPECT_EQ(columns[0].first, 0);
        EXPECT_EQ(columns[0].second, 0);
        EXPECT_EQ(columns[1].first, 0);
        EXPECT_EQ(columns[1].second, 1);
        EXPECT_EQ(columns[2].first, 0);
        EXPECT_EQ(columns[2].second, 2);
        EXPECT_EQ(turned_states, 3);
    }

    TEST(JigsawTest, AssemblyTurnsPiecesToFaceTheirConnections) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        using terila::world::worldgen::assemble_jigsaw;

        const auto resolve = [](std::string_view) { return 0; };
        const auto rotate = [](std::int32_t state, int) { return state; };
        // With a rotation table the assembly may lay a piece down turned, which is how a
        // house that does not already face a street still joins it.
        const auto pieces = assemble_jigsaw("minecraft:village/plains/town_centers",
            resolve, 0, 70, 0, 1337, 64, 4, 80, rotate);
        ASSERT_GT(pieces.size(), 0u);
        int turned = 0;
        for (const auto& piece : pieces) {
            if (piece.rotation != 0) {
                ++turned;
            }
        }
        EXPECT_GT(turned, 0) << "pieces " << pieces.size();

        // Without the table nothing may be turned: its cells would keep their unturned states.
        const auto plain = assemble_jigsaw("minecraft:ancient_city/city_center",
            resolve, 0, -27, 0, 1337, 64, 4, 116);
        for (const auto& piece : plain) {
            EXPECT_EQ(piece.rotation, 0);
        }
    }

    TEST(JigsawTest, TerrainMatchingPiecesSitOnTheirOwnGroundAndFillBeneath) {
        using terila::world::worldgen::PlacedPiece;
        using terila::world::worldgen::write_jigsaw_pieces;

        // A four by four floor piece: one layer of blocks with nothing above it.
        const auto make_floor = [](std::int32_t origin_y, bool terrain_matching) {
            PlacedPiece piece;
            piece.tmpl.size_x = 4;
            piece.tmpl.size_y = 1;
            piece.tmpl.size_z = 4;
            piece.origin_y = origin_y;
            piece.terrain_matching = terrain_matching;
            for (std::int32_t x = 0; x < 4; ++x) {
                for (std::int32_t z = 0; z < 4; ++z) {
                    piece.tmpl.cells.push_back({ x, 0, z, 7 });
                }
            }
            return piece;
        };

        // The ground rises four blocks from the west edge of the piece to the east one.
        const auto surface = [](std::int32_t x, std::int32_t) {
            return x < 2 ? 70 : 74;
        };
        const std::int32_t filler = 9;

        std::vector<std::pair<std::int32_t, std::int32_t>> floor;
        int filled = 0;
        write_jigsaw_pieces(std::vector<PlacedPiece>{ make_floor(70, true) }, 0, 0, surface,
            filler, {}, [&](std::int32_t x, std::int32_t y, std::int32_t, std::int32_t state) {
                if (state == filler) {
                    ++filled;
                    return;
                }
                floor.emplace_back(x, y);
            });

        // The floor drops onto the height under the piece instead of staying at the height
        // its connector gave it.
        ASSERT_EQ(floor.size(), 16u);
        for (const auto& [x, y] : floor) {
            EXPECT_EQ(y, 74) << "column " << x << " stayed at " << y;
        }
        // The two western columns hang three blocks over the lower ground, and that gap is
        // filled rather than left empty.
        EXPECT_EQ(filled, 2 * 4 * 3);

        // A rigid piece keeps the height its connector gave it, and the gap under it is
        // filled rather than left empty.
        std::vector<std::pair<std::int32_t, std::int32_t>> rigid_floor;
        int rigid_filled = 0;
        write_jigsaw_pieces(std::vector<PlacedPiece>{ make_floor(80, false) }, 0, 0, surface,
            filler, {}, [&](std::int32_t x, std::int32_t y, std::int32_t, std::int32_t state) {
                if (state == filler) {
                    ++rigid_filled;
                    return;
                }
                rigid_floor.emplace_back(x, y);
            });
        ASSERT_EQ(rigid_floor.size(), 16u);
        for (const auto& [x, y] : rigid_floor) {
            EXPECT_EQ(y, 80) << "column " << x << " moved to " << y;
        }
        EXPECT_GT(rigid_filled, 0);
    }

    TEST(ProcessorTest, MossyRuleRewritesCobblestone) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        using terila::world::worldgen::apply_processors;
        using terila::world::worldgen::load_processor_list;
        const auto resolve = [](std::string_view text) {
            const std::string name(text);
            if (name == "minecraft:cobblestone") return 5;
            if (name == "minecraft:mossy_cobblestone") return 6;
            return 0;
        };
        auto list = load_processor_list("minecraft:mossify_70_percent", resolve);
        ASSERT_TRUE(list.has_value());
        ASSERT_FALSE(list->rules.empty());

        terila::world::worldgen::StructureTemplate tmpl;
        tmpl.size_x = 100;
        tmpl.size_y = 1;
        tmpl.size_z = 10;
        for (int index = 0; index < 1000; ++index) {
            terila::world::worldgen::TemplateCell cell;
            cell.x = index % 100;
            cell.z = index / 100;
            cell.state = 5;
            tmpl.cells.push_back(cell);
        }
        std::uint64_t rng = 12345;
        apply_processors(tmpl, *list, rng);
        int mossy = 0;
        int plain = 0;
        for (const auto& cell : tmpl.cells) {
            (cell.state == 6 ? mossy : plain) += 1;
        }
        EXPECT_GT(mossy, 550);
        EXPECT_LT(mossy, 850);
        EXPECT_GT(plain, 150);
    }

    TEST(ProcessorTest, RotRemovesAboutTheMissingIntegrity) {
        if (!input_available()) {
            GTEST_SKIP() << "input/ is not next to the build tree";
        }
        using terila::world::worldgen::apply_processors;
        using terila::world::worldgen::load_processor_list;
        // fossil_rot keeps 90% of its blocks; nothing is protected here, because the
        // resolver sends every tag member to air and the cells are state 5.
        const auto resolve = [](std::string_view) { return 0; };
        auto list = load_processor_list("minecraft:fossil_rot", resolve);
        ASSERT_TRUE(list.has_value());
        ASSERT_TRUE(list->has_rot);

        terila::world::worldgen::StructureTemplate tmpl;
        tmpl.size_x = 100;
        tmpl.size_y = 1;
        tmpl.size_z = 10;
        for (int index = 0; index < 1000; ++index) {
            terila::world::worldgen::TemplateCell cell;
            cell.x = index % 100;
            cell.z = index / 100;
            cell.state = 5;
            tmpl.cells.push_back(cell);
        }
        std::uint64_t rng = 777;
        apply_processors(tmpl, *list, rng);
        int air = 0;
        for (const auto& cell : tmpl.cells) {
            if (cell.state == 0) {
                ++air;
            }
        }
        EXPECT_GT(air, 40);
        EXPECT_LT(air, 250);
    }

} // namespace
