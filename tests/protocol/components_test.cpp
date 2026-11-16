#include <gtest/gtest.h>

#include "item/components.hpp"
#include "nbt/nbt_codec.hpp"
#include "nbt/nbt_text.hpp"
#include "protocol/item.hpp"
#include "protocol/varint.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using terila::item::ComponentIds;
using terila::item::Components;

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
    const std::unordered_map<std::string, std::int32_t> enchantment_registry{
        { "minecraft:efficiency", 5 }
    };
    ComponentIds ids;
    ids.enchantments = 13;
    ids.custom_name = 6;
    ids.enchantment_registry = &enchantment_registry;

    std::vector<std::uint8_t> out;
    terila::protocol::write_item_stack(out, 1050, 1, &components, &ids);

    EXPECT_EQ(out, (std::vector<std::uint8_t>{
        1, 0x9A, 0x08, 1, 0, 13, 1, 5, 0xFF, 0x01
    }));
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
    components.set_enchantment("minecraft:sharpness", 255);
    components.custom_name = "Diamond Sword";
    const std::unordered_map<std::string, std::int32_t> enchantment_registry{
        { "minecraft:sharpness", 9 }
    };
    ComponentIds ids;
    ids.enchantments = 13;
    ids.custom_name = 6;
    ids.enchantment_registry = &enchantment_registry;

    std::vector<std::uint8_t> out;
    terila::protocol::write_item_stack(out, 1050, 1, &components, &ids);

    const std::vector<std::uint8_t> header{ 1, 0x9A, 0x08, 2, 0, 13, 1, 9, 0xFF, 0x01, 6 };
    ASSERT_GE(out.size(), header.size());
    EXPECT_TRUE(std::equal(header.begin(), header.end(), out.begin()));

    terila::nbt::NbtReader name_reader(std::span<const std::uint8_t>(out.data() + header.size(),
        out.size() - header.size()));
    auto name_root = name_reader.root_map();
    ASSERT_TRUE(name_root);
    const terila::nbt::NbtValue root_value(std::move(*name_root));
    const auto* entry = terila::nbt::at(root_value, "text");
    ASSERT_NE(entry, nullptr);
    const auto* text = entry->get_if<std::string>();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(*text, "Diamond Sword");
}
