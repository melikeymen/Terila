#include "nbt/nbt.hpp"

#include <gtest/gtest.h>

#include <type_traits>

using namespace terila::nbt;

TEST(Nbt, RoundTripNestedAndMixedValues) {
    NbtCompound root;
    root["byte"] = std::int8_t{-3};
    root["short"] = std::int16_t{32000};
    root["int"] = std::int32_t{123456};
    root["long"] = std::int64_t{9876543210LL};
    root["float"] = 1.25f;
    root["double"] = 3.5;
    root["string"] = std::string("hello");
    root["bytes"] = NbtByteArray{1, -2};
    root["ints"] = NbtIntArray{1, -2};
    root["longs"] = NbtLongArray{3, -4};
    root["child"] = NbtCompound{{"middle", NbtCompound{{"leaf", std::int32_t{9}}}}};
    root["list"] = NbtList{NbtValue(NbtCompound{{"v", std::int32_t{1}}}), NbtValue(NbtCompound{{"v", std::int32_t{2}}})};
    for (auto format : {StringFormat::Network, StringFormat::Classic}) {
        Writer writer(format);
        writer.begin_root_compound();
        for (const auto& [name, value] : root) writer.write_named_value(name, value);
        writer.end_compound();
        auto bytes = writer.finish();
        Reader reader(bytes, format);
        auto parsed = reader.read_root_compound();
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, root);
        EXPECT_EQ(reader.position(), bytes.size());
    }
}

TEST(Nbt, EmptyCompoundAndList) {
    NbtCompound root{{"empty", NbtCompound{}}, {"list", NbtList{}}};
    Writer writer;
    writer.begin_root_compound();
    for (const auto& [name, value] : root) writer.write_named_value(name, value);
    writer.end_compound();
    auto bytes = writer.finish();
    Reader reader(bytes);
    auto parsed = reader.read_root_compound();
    ASSERT_TRUE(parsed); EXPECT_EQ(*parsed, root);
}

TEST(Nbt, RejectsMalformedInput) {
    const std::vector<std::uint8_t> truncated_bytes{10, 8, 0, 1};
    Reader truncated(truncated_bytes);
    EXPECT_FALSE(truncated.read_root_compound());
    const std::vector<std::uint8_t> invalid_bytes{10, 99};
    Reader invalid(invalid_bytes);
    EXPECT_FALSE(invalid.read_root_compound());
    std::vector<std::uint8_t> oversized{10, 8, 0, 0xff, 0xff, 0xff, 0xff, 0x07};
    Reader string_reader(oversized);
    EXPECT_FALSE(string_reader.read_root_compound());
}

TEST(Nbt, LegacyTextComponentBytesRemainClassic) {
    std::vector<std::uint8_t> bytes;
    write_text_component(bytes, "hello");
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{10, 0, 0, 8, 0, 4, 't', 'e', 'x', 't', 0, 5, 'h', 'e', 'l', 'l', 'o', 0}));
}

TEST(Nbt, RejectsMixedTypeList) {
    NbtList mixed{NbtValue(std::int32_t{1}), NbtValue(std::string("x"))};
    Writer writer;
    writer.begin_root_compound();
    writer.write_named_value("mixed", NbtValue(std::move(mixed)));
    writer.end_compound();
    EXPECT_TRUE(writer.finish().empty());
}

TEST(Nbt, NoSilentNarrowing) {
    static_assert(!std::is_constructible_v<NbtValue, unsigned int>);
    NbtValue value(300);
    const auto* integer = value.get_if<std::int32_t>();
    ASSERT_NE(integer, nullptr);
    EXPECT_EQ(*integer, 300);
}

TEST(Nbt, RejectsLongString) {
    Writer writer(StringFormat::Classic);
    writer.begin_root_compound();
    writer.write_string("long", std::string(70000, 'x'));
    writer.end_compound();
    EXPECT_TRUE(writer.finish().empty());
}

TEST(Nbt, RejectsDeepRecursion) {
    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(Tag::Compound)};
    for (int i = 0; i < 512; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(Tag::Compound));
        bytes.push_back(0);
    }
    bytes.insert(bytes.end(), 513, static_cast<std::uint8_t>(Tag::End));
    Reader reader(bytes);
    EXPECT_FALSE(reader.read_root_compound());
}

TEST(Nbt, RoundTripHomogeneousList) {
    NbtList values{NbtValue(std::int32_t{1}), NbtValue(std::int32_t{2}), NbtValue(std::int32_t{3})};
    Writer writer;
    writer.begin_root_compound();
    writer.write_named_value("values", NbtValue(values));
    writer.end_compound();
    auto bytes = writer.finish();
    Reader reader(bytes);
    auto parsed = reader.read_root_compound();
    ASSERT_TRUE(parsed);
    ASSERT_NE(parsed->find("values"), parsed->end());
    EXPECT_EQ(*parsed->at("values").get_if<NbtList>(), values);
}
