#include <gtest/gtest.h>

#include "item/components.hpp"
#include "protocol/item.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using terila::item::ComponentIds;
using terila::item::Components;

namespace {

    bool contains_sequence(const std::vector<std::uint8_t>& haystack,
        const std::vector<std::uint8_t>& needle) {
        if (needle.empty() || haystack.size() < needle.size()) {
            return false;
        }
        for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
            if (std::equal(needle.begin(), needle.end(), haystack.begin() + i)) {
                return true;
            }
        }
        return false;
    }

    // "minecraft:efficiency" as an NBT int tag: tag byte, name length, name, value.
    std::vector<std::uint8_t> efficiency_tag(std::int32_t level) {
        const std::string name = "minecraft:efficiency";
        std::vector<std::uint8_t> tag{ 0x03 };
        tag.push_back(static_cast<std::uint8_t>(name.size() >> 8));
        tag.push_back(static_cast<std::uint8_t>(name.size() & 0xFF));
        tag.insert(tag.end(), name.begin(), name.end());
        tag.push_back(static_cast<std::uint8_t>((level >> 24) & 0xFF));
        tag.push_back(static_cast<std::uint8_t>((level >> 16) & 0xFF));
        tag.push_back(static_cast<std::uint8_t>((level >> 8) & 0xFF));
        tag.push_back(static_cast<std::uint8_t>(level & 0xFF));
        return tag;
    }

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
    const ComponentIds ids{ 13, 6 };

    std::vector<std::uint8_t> out;
    terila::protocol::write_item_stack(out, 1050, 1, &components, &ids);

    // count, item, one added component of type 13 ...
    const std::vector<std::uint8_t> header{ 1, 0x9A, 0x08, 1, 13 };
    ASSERT_GE(out.size(), header.size());
    EXPECT_TRUE(std::equal(header.begin(), header.end(), out.begin()));

    // ... a root compound holding levels -> { efficiency: 255 } ...
    EXPECT_EQ(out[5], 0x0A);                 // TAG_Compound (root)
    EXPECT_EQ(out[6], 0x0A);                 // TAG_Compound "levels"
    EXPECT_TRUE(contains_sequence(out, efficiency_tag(255)));

    // ... then "levels" and the root close, and no component is removed.
    ASSERT_GE(out.size(), 3u);
    EXPECT_EQ(out[out.size() - 3], 0x00);
    EXPECT_EQ(out[out.size() - 2], 0x00);
    EXPECT_EQ(out[out.size() - 1], 0x00);
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

    // A text component is an unnamed compound with a single "text" string.
    EXPECT_TRUE(contains_sequence(out, { 6, 0x0A, 0x08, 0x00, 0x04, 't', 'e', 'x', 't' }));
}
