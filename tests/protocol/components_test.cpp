#include <gtest/gtest.h>

#include "item/components.hpp"
#include "nbt/nbt_reader.hpp"
#include "protocol/item.hpp"
#include "protocol/string.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using terila::item::ComponentIds;
using terila::item::Components;

namespace {

} // namespace

TEST(ItemComponentsTest, ComponentIdsComeFromTheRegistry) {
    const std::unordered_map<std::string, std::int32_t> registry{
        { "minecraft:enchantments", 13 },
        { "minecraft:custom_name", 6 },
    };
    const auto ids = terila::item::component_ids(registry);
    EXPECT_EQ(ids.enchantments, 13);
    EXPECT_EQ(ids.custom_name, 6);
    EXPECT_TRUE(ids.usable());
}

TEST(ItemComponentsTest, MissingRegistryLeavesIdsUnusable) {
    const auto ids = terila::item::component_ids({});
    EXPECT_FALSE(ids.usable());
}

TEST(ItemComponentsTest, EmptyStackIsOnlyACount) {
    std::vector<std::uint8_t> out;
    terila::protocol::write_item_stack(out, 1050, 0);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], 0);
}

TEST(ItemComponentsTest, PlainStackHasAnEmptyPatch) {
    std::vector<std::uint8_t> out;
    terila::protocol::write_item_stack(out, 1050, 1, nullptr, nullptr);
    // count, item id (varint 1050), added count 0, removed count 0
    const std::vector<std::uint8_t> expected{ 1, 0x9A, 0x08, 0, 0 };
    EXPECT_EQ(out, expected);
}

TEST(ItemComponentsTest, EfficiencyPatchCarriesTheLevel) {
    Components components;
    components.set_enchantment("minecraft:efficiency", 255);
    ComponentIds ids{ 13, 6 };
    const std::unordered_map<std::string, std::int32_t> enchantment_registry{
        { "minecraft:efficiency", 5 }
    };
    ids.enchantment_registry = &enchantment_registry;

    std::vector<std::uint8_t> out;
    terila::protocol::write_item_stack(out, 1050, 1, &components, &ids);

    const std::string component_name = "minecraft:enchantments";
    ASSERT_EQ(component_name.size(), 22u);
    ASSERT_EQ(out.size(), 32u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[3], 1);
    EXPECT_EQ(out[4], component_name.size());
    EXPECT_TRUE(std::equal(component_name.begin(), component_name.end(), out.begin() + 5));
    EXPECT_EQ(out[27], 1);
    EXPECT_EQ(out[28], 5);
    EXPECT_EQ(out[29], 0xFF);
    EXPECT_EQ(out[30], 0x01);
    EXPECT_EQ(out[31], 0);
}

TEST(ItemComponentsTest, NullAndEmptyPatchesStackTogether) {
    const terila::item::ComponentsPtr none;
    const auto empty = std::make_shared<Components>();
    EXPECT_TRUE(terila::item::same_components(none, empty));
    EXPECT_TRUE(terila::item::same_components(empty, none));
    EXPECT_TRUE(terila::item::same_components(none, none));
}

TEST(ItemComponentsTest, EqualValuesStackButDifferentEnchantmentsDoNot) {
    auto first = std::make_shared<Components>();
    first->set_enchantment("minecraft:efficiency", 255);
    auto second = std::make_shared<Components>();
    second->set_enchantment("minecraft:efficiency", 255);
    auto other = std::make_shared<Components>();
    other->set_enchantment("minecraft:efficiency", 1);

    // Same values in different allocations still stack; the pointer compare would not.
    EXPECT_TRUE(terila::item::same_components(first, second));
    EXPECT_FALSE(terila::item::same_components(first, other));
    EXPECT_FALSE(terila::item::same_components(first, nullptr));
}

TEST(ItemComponentsTest, SetEnchantmentOverwritesInPlace) {
    Components components;
    components.set_enchantment("minecraft:efficiency", 10);
    components.set_enchantment("minecraft:efficiency", 255);
    ASSERT_EQ(components.enchantments.size(), 1u);
    EXPECT_EQ(components.enchantment_level("minecraft:efficiency").value_or(0), 255);
}

TEST(ItemComponentsTest, CustomNameAddsASecondComponent) {
    Components components;
    components.custom_name = "Diamond Sword";
    const ComponentIds ids{ -1, 6 };

    std::vector<std::uint8_t> out;
    terila::protocol::write_item_stack(out, 1050, 1, &components, &ids);

    const std::vector<std::uint8_t> header{ 1, 0x9A, 0x08, 1, 21 };
    ASSERT_GE(out.size(), header.size());
    EXPECT_TRUE(std::equal(header.begin(), header.end(), out.begin()));
    const auto component_name = terila::protocol::read_string(out.data() + 4,
        out.size() - 4);
    ASSERT_TRUE(component_name);
    EXPECT_EQ(component_name->value, "minecraft:custom_name");
    const auto nbt_offset = 4 + component_name->consumed;
    terila::nbt::Reader reader(std::span<const std::uint8_t>(out.data() + nbt_offset,
        out.size() - nbt_offset - 1));
    auto root = reader.read_root_compound();
    ASSERT_TRUE(root);
    const auto* text = root->at("text").get_if<std::string>();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(*text, "Diamond Sword");
}
