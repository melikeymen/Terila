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

} // namespace
