#include <gtest/gtest.h>

#include "item/item_parser.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace item = terila::item;

namespace {

    struct Registry {
        std::unordered_map<std::string, std::int32_t> components{
            { "minecraft:enchantments", 1 },
            { "minecraft:custom_name", 2 },
            { "minecraft:item_name", 3 },
            { "minecraft:damage", 4 },
            { "minecraft:max_stack_size", 5 },
            { "minecraft:unbreakable", 6 },
        };
        std::unordered_map<std::string, std::int32_t> enchantments{
            { "minecraft:sharpness", 0 },
            { "minecraft:efficiency", 1 },
        };

        item::ComponentIds ids() const {
            return item::component_ids(components, &enchantments);
        }
    };

} // namespace

TEST(ItemParserTest, ReadsAPlainItem) {
    const Registry registry;
    std::string error;
    const auto parsed = item::parse_item("diamond_sword", registry.ids(), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->id, "minecraft:diamond_sword");
    EXPECT_TRUE(parsed->components.empty());
}

TEST(ItemParserTest, ReadsEnchantments) {
    const Registry registry;
    std::string error;
    const auto parsed = item::parse_item(
        "diamond_sword[minecraft:enchantments={levels:{\"minecraft:sharpness\":5}}]",
        registry.ids(), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    ASSERT_EQ(parsed->components.enchantments.size(), 1u);
    EXPECT_EQ(parsed->components.enchantments.front().id, "minecraft:sharpness");
    EXPECT_EQ(parsed->components.enchantments.front().level, 5);
}

TEST(ItemParserTest, ReadsUnitAndRemovedComponents) {
    const Registry registry;
    std::string error;
    const auto parsed = item::parse_item("diamond_sword[minecraft:unbreakable={},!minecraft:damage]",
        registry.ids(), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_TRUE(parsed->components.unbreakable);
    ASSERT_EQ(parsed->components.removed.size(), 1u);
    EXPECT_EQ(parsed->components.removed.front(), 4);   // minecraft:damage
}

TEST(ItemParserTest, WrapsAPlainNameAsATextComponent) {
    const Registry registry;
    std::string error;
    const auto parsed = item::parse_item("diamond_sword[minecraft:custom_name=\"Big Sword\"]",
        registry.ids(), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    ASSERT_FALSE(parsed->components.custom_name_nbt.empty());
    EXPECT_EQ(parsed->components.custom_name_nbt.front(), 0x0A);   // a compound tag
}

TEST(ItemParserTest, ReadsIntegerComponents) {
    const Registry registry;
    std::string error;
    const auto parsed = item::parse_item(
        "diamond_sword[minecraft:damage=10,minecraft:max_stack_size=1]", registry.ids(), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    ASSERT_TRUE(parsed->components.damage.has_value());
    EXPECT_EQ(*parsed->components.damage, 10);
    ASSERT_TRUE(parsed->components.max_stack_size.has_value());
    EXPECT_EQ(*parsed->components.max_stack_size, 1);
}

TEST(ItemParserTest, ReportsUnknownComponentsAndEnchantments) {
    const Registry registry;
    std::string error;
    EXPECT_FALSE(item::parse_item("diamond_sword[minecraft:nope=1]", registry.ids(), error)
        .has_value());
    EXPECT_NE(error.find("unknown component"), std::string::npos);

    error.clear();
    EXPECT_FALSE(item::parse_item(
        "diamond_sword[minecraft:enchantments={levels:{\"minecraft:nope\":1}}]", registry.ids(),
        error)
        .has_value());
    EXPECT_NE(error.find("unknown enchantment"), std::string::npos);
}

// An empty levels map is not "no enchantments component": it is an empty one, which
// clears whatever the item had by default.
TEST(ItemParserTest, EmptyEnchantmentLevelsStillSendTheComponent) {
    const Registry registry;
    std::string error;
    const auto parsed = item::parse_item("diamond_sword[minecraft:enchantments={levels:{}}]",
        registry.ids(), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_TRUE(parsed->components.enchantments.empty());
    EXPECT_TRUE(parsed->components.enchantments_present);
    EXPECT_FALSE(parsed->components.empty());
}

TEST(ItemParserTest, RejectsAComponentTheWriterCannotEncode) {
    const Registry registry;
    std::string error;
    const std::string huge(70'000, 'a');
    const auto parsed = item::parse_item("diamond_sword[minecraft:custom_name=\"" + huge + "\"]",
        registry.ids(), error);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_NE(error.find("cannot encode"), std::string::npos);
}

TEST(ItemParserTest, RejectsAMalformedComponentList) {
    const Registry registry;
    std::string error;
    EXPECT_FALSE(item::parse_item("diamond_sword[minecraft:damage=1", registry.ids(), error)
        .has_value());
    EXPECT_FALSE(item::parse_item("diamond_sword[minecraft:damage]", registry.ids(), error)
        .has_value());
}

TEST(ItemParserTest, ReadsTheCompactEnchantmentMap) {
    const Registry registry;
    std::string error;
    // The spelling players actually type: no "levels" wrapper, and the name is optional
    // about its namespace.
    const auto parsed = item::parse_item(
        "diamond_sword[enchantments={sharpness:255}]", registry.ids(), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    ASSERT_EQ(parsed->components.enchantments.size(), 1u);
    EXPECT_EQ(parsed->components.enchantment_level("minecraft:sharpness").value_or(0), 255);
}

TEST(ItemParserTest, ReadsAnEmptyEnchantmentMap) {
    const Registry registry;
    std::string error;
    const auto parsed = item::parse_item(
        "diamond_sword[enchantments={}]", registry.ids(), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_TRUE(parsed->components.enchantments.empty());
    EXPECT_TRUE(parsed->components.enchantments_present);
}
