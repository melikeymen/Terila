#include <gtest/gtest.h>

#include "nbt/snbt.hpp"

#include <cstdint>

namespace nbt = terila::nbt;

TEST(SnbtTest, ParsesTheEnchantmentLevelsOfAGiveCommand) {
    const auto value = nbt::parse_snbt("{levels:{\"minecraft:sharpness\":5}}");
    ASSERT_TRUE(value.has_value());
    const auto* compound = value->get_if<nbt::NbtCompound>();
    ASSERT_NE(compound, nullptr);

    const auto levels = compound->find("levels");
    ASSERT_NE(levels, compound->end());
    const auto* entries = levels->second.get_if<nbt::NbtCompound>();
    ASSERT_NE(entries, nullptr);

    const auto sharpness = entries->find("minecraft:sharpness");
    ASSERT_NE(sharpness, entries->end());
    const auto* level = sharpness->second.get_if<std::int32_t>();
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(*level, 5);
}

TEST(SnbtTest, ParsesAnEmptyCompound) {
    const auto value = nbt::parse_snbt("{}");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->tag(), nbt::Tag::Compound);
    ASSERT_NE(value->get_if<nbt::NbtCompound>(), nullptr);
    EXPECT_TRUE(value->get_if<nbt::NbtCompound>()->empty());
}

TEST(SnbtTest, KeepsNumbersInTheirTypedForm) {
    EXPECT_EQ(nbt::parse_snbt("5")->tag(), nbt::Tag::Int);
    EXPECT_EQ(nbt::parse_snbt("5b")->tag(), nbt::Tag::Byte);
    EXPECT_EQ(nbt::parse_snbt("5s")->tag(), nbt::Tag::Short);
    EXPECT_EQ(nbt::parse_snbt("5L")->tag(), nbt::Tag::Long);
    EXPECT_EQ(nbt::parse_snbt("1.5")->tag(), nbt::Tag::Double);
    EXPECT_EQ(nbt::parse_snbt("1.5f")->tag(), nbt::Tag::Float);
    EXPECT_EQ(nbt::parse_snbt("true")->tag(), nbt::Tag::Byte);
    // Too wide for an int, so it becomes a long the way the game does it.
    EXPECT_EQ(nbt::parse_snbt("3000000000")->tag(), nbt::Tag::Long);
}

TEST(SnbtTest, ReadsQuotedStringsAndBareWords) {
    const auto quoted = nbt::parse_snbt("\"Big Sword\"");
    ASSERT_TRUE(quoted.has_value());
    ASSERT_NE(quoted->get_if<std::string>(), nullptr);
    EXPECT_EQ(*quoted->get_if<std::string>(), "Big Sword");

    const auto bare = nbt::parse_snbt("minecraft:efficiency");
    ASSERT_TRUE(bare.has_value());
    ASSERT_NE(bare->get_if<std::string>(), nullptr);
    EXPECT_EQ(*bare->get_if<std::string>(), "minecraft:efficiency");
}

TEST(SnbtTest, ReadsListsAndTypedArrays) {
    const auto list = nbt::parse_snbt("[1,2,3]");
    ASSERT_TRUE(list.has_value());
    ASSERT_NE(list->get_if<nbt::NbtList>(), nullptr);
    EXPECT_EQ(list->get_if<nbt::NbtList>()->size(), 3);

    const auto ints = nbt::parse_snbt("[I;1,2]");
    ASSERT_TRUE(ints.has_value());
    ASSERT_NE(ints->get_if<nbt::NbtIntArray>(), nullptr);
    EXPECT_EQ(ints->get_if<nbt::NbtIntArray>()->size(), 2);

    const auto bytes = nbt::parse_snbt("[B;1b,2b]");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->tag(), nbt::Tag::ByteArray);
}

TEST(SnbtTest, RejectsTrailingTextAndMixedLists) {
    EXPECT_FALSE(nbt::parse_snbt("{a:1} extra").has_value());
    EXPECT_FALSE(nbt::parse_snbt("{a:1").has_value());
    EXPECT_FALSE(nbt::parse_snbt("[1,\"two\"]").has_value());   // one element type per list
}

TEST(SnbtTest, RejectsDeepNesting) {
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text += "{a:";
    }
    text += "1";
    for (int i = 0; i < 200; ++i) {
        text += "}";
    }
    EXPECT_FALSE(nbt::parse_snbt(text).has_value());
}
