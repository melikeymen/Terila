#include <gtest/gtest.h>

#include "item/components.hpp"
#include "nbt/nbt_reader.hpp"
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
    const ComponentIds ids{ 13, 6 };

    std::vector<std::uint8_t> out;
    terila::protocol::write_item_stack(out, 1050, 1, &components, &ids);

    // count, item, one added component of type 13 ...
    const std::vector<std::uint8_t> header{ 1, 0x9A, 0x08, 1, 13 };
    ASSERT_GE(out.size(), header.size());
    EXPECT_TRUE(std::equal(header.begin(), header.end(), out.begin()));

    ASSERT_GT(out.size(), 6u);
    const auto nbt_size = out.size() - 6;
    terila::nbt::Reader reader(std::span<const std::uint8_t>(out.data() + 5, nbt_size));
    auto root = reader.read_root_compound();
    ASSERT_TRUE(root);
    const auto* levels = root->at("levels").get_if<terila::nbt::NbtCompound>();
    ASSERT_NE(levels, nullptr);
    const auto* level = levels->at("minecraft:efficiency").get_if<std::int32_t>();
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(*level, 255);
    EXPECT_EQ(reader.position(), nbt_size);
    EXPECT_EQ(out[out.size() - 1], 0);
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
    const ComponentIds ids{ 13, 6 };

    std::vector<std::uint8_t> out;
    terila::protocol::write_item_stack(out, 1050, 1, &components, &ids);

    const std::vector<std::uint8_t> header{ 1, 0x9A, 0x08, 2, 13 };
    ASSERT_GE(out.size(), header.size());
    EXPECT_TRUE(std::equal(header.begin(), header.end(), out.begin()));

    ASSERT_GT(out.size(), 6u);
    const auto nbt_size = out.size() - 6;
    terila::nbt::Reader first_reader(std::span<const std::uint8_t>(out.data() + 5, nbt_size));
    auto first = first_reader.read_root_compound();
    ASSERT_TRUE(first);
    const auto first_size = first_reader.position();
    const auto second_component_offset = 5 + first_size;
    const auto second_type = terila::protocol::read_varint(out.data() + second_component_offset,
        nbt_size - first_size);
    ASSERT_TRUE(second_type);
    EXPECT_EQ(second_type->value, 6);
    terila::nbt::Reader name_reader(std::span<const std::uint8_t>(
        out.data() + second_component_offset + second_type->consumed,
        nbt_size - first_size - second_type->consumed));
    auto name_root = name_reader.read_root_compound();
    ASSERT_TRUE(name_root);
    const auto* text = name_root->at("text").get_if<std::string>();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(*text, "Diamond Sword");
}
