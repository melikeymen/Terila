#include <gtest/gtest.h>

#include "tag/text.hpp"

#include <cstdint>

namespace tag = terila::tag;

TEST(TagTextTest, ParsesTheEnchantmentMapOfAGiveCommand) {
    const auto value = tag::parse_tag("{levels:{\"minecraft:sharpness\":5}}");
    ASSERT_TRUE(value.has_value());
    const auto* outer = value->get_if<tag::TagMap>();
    ASSERT_NE(outer, nullptr);

    const auto* entries = tag::at(*value, "levels");
    ASSERT_NE(entries, nullptr);
    EXPECT_EQ(tag::int_at(*entries, "minecraft:sharpness"), 5);
}

TEST(TagTextTest, ReadsADirectEnchantmentMap) {
    const auto value = tag::parse_tag("{sharpness:255}");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(tag::int_at(*value, "sharpness"), 255);
}

TEST(TagTextTest, ParsesAnEmptyMap) {
    const auto value = tag::parse_tag("{}");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->tag(), tag::TagKind::Map);
    ASSERT_NE(value->get_if<tag::TagMap>(), nullptr);
    EXPECT_TRUE(value->get_if<tag::TagMap>()->empty());
}

TEST(TagTextTest, RoundTripsThroughText) {
    const auto parsed = tag::parse_tag("{sharpness:255,name:\"Big Sword\",x:1b}");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(tag::to_text(*parsed), "{name:\"Big Sword\",sharpness:255,x:1b}");
}
