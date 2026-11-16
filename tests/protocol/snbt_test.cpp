#include <gtest/gtest.h>

#include "nbt/nbt_text.hpp"

#include <cstdint>

namespace tag = terila::nbt;

TEST(TagTextTest, ParsesTheEnchantmentMapOfAGiveCommand) {
    const auto value = tag::parse_snbt("{levels:{\"minecraft:sharpness\":5}}");
    ASSERT_TRUE(value.has_value());
    const auto* outer = value->get_if<tag::NbtCompound>();
    ASSERT_NE(outer, nullptr);

    const auto* entries = tag::at(*value, "levels");
    ASSERT_NE(entries, nullptr);
    EXPECT_EQ(tag::int_at(*entries, "minecraft:sharpness"), 5);
}

TEST(TagTextTest, ReadsADirectEnchantmentMap) {
    const auto value = tag::parse_snbt("{sharpness:255}");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(tag::int_at(*value, "sharpness"), 255);
}

TEST(TagTextTest, ParsesAnEmptyMap) {
    const auto value = tag::parse_snbt("{}");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->tag(), tag::Tag::Map);
    ASSERT_NE(value->get_if<tag::NbtCompound>(), nullptr);
    EXPECT_TRUE(value->get_if<tag::NbtCompound>()->empty());
}

TEST(TagTextTest, RoundTripsThroughText) {
    const auto parsed = tag::parse_snbt("{sharpness:255,name:\"Big Sword\",x:1b}");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(tag::to_snbt(*parsed), "{name:\"Big Sword\",sharpness:255,x:1b}");
}
