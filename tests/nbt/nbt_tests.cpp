#include "nbt/nbt.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

using namespace terila::nbt;

namespace {

    NbtCompound sample_tree() {
        NbtCompound root;
        root["byte"] = std::int8_t{ -3 };
        root["short"] = std::int16_t{ 32000 };
        root["int"] = std::int32_t{ 123456 };
        root["long"] = std::int64_t{ 9876543210LL };
        root["float"] = 1.25f;
        root["double"] = 3.5;
        root["text"] = std::string("hello");
        root["bytes"] = NbtBytes{ 1, 254 };
        root["ints"] = NbtInts{ 1, -2 };
        root["longs"] = NbtLongs{ 3, -4 };
        root["child"] = NbtCompound{ { "middle", NbtCompound{ { "leaf", std::int32_t{ 9 } } } } };
        root["list"] = NbtList{
            NbtValue(NbtCompound{ { "v", std::int32_t{ 1 } } }),
            NbtValue(NbtCompound{ { "v", std::int32_t{ 2 } } })
        };
        return root;
    }

    std::vector<std::uint8_t> encode_tree(const NbtCompound& root, Layout layout) {
        NbtWriter writer(layout);
        writer.open_map();
        for (const auto& [name, value] : root) {
            writer.put(name, value);
        }
        writer.close_map();
        return writer.finish();
    }

} // namespace

TEST(TagCodec, RoundTripsEveryShapeInBothLayouts) {
    const auto root = sample_tree();
    for (const auto layout : { Layout::Unnamed, Layout::Named }) {
        const auto bytes = encode_tree(root, layout);
        ASSERT_FALSE(bytes.empty());
        NbtReader reader(bytes, layout);
        auto parsed = reader.root_map();
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, root);
        EXPECT_EQ(reader.position(), bytes.size());
    }
}

TEST(TagCodec, KeepsEmptyContainers) {
    const NbtCompound root{ { "empty", NbtCompound{} }, { "list", NbtList{} } };
    const auto bytes = encode_tree(root, Layout::Unnamed);
    NbtReader reader(bytes);
    auto parsed = reader.root_map();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, root);
}

TEST(TagCodec, RefusesTruncatedAndUnknownPayloads) {
    const std::vector<std::uint8_t> truncated{ 10, 8, 0, 1 };
    NbtReader first(truncated);
    EXPECT_FALSE(first.root_map().has_value());

    const std::vector<std::uint8_t> unknown_kind{ 10, 99 };
    NbtReader second(unknown_kind);
    EXPECT_FALSE(second.root_map().has_value());

    const std::vector<std::uint8_t> huge_text{ 10, 8, 0, 0xff, 0xff, 0xff, 0xff, 0x07 };
    NbtReader third(huge_text);
    EXPECT_FALSE(third.root_map().has_value());
}

TEST(TagCodec, WritesAChatComponentAsAnUnnamedValue) {
    const auto bytes = text_component_nbt("hello");
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{
        10, 8, 0, 4, 't', 'e', 'x', 't', 0, 5, 'h', 'e', 'l', 'l', 'o', 0 }));
}

TEST(TagCodec, RefusesAListThatMixesTypes) {
    NbtCompound root;
    root["mixed"] = NbtList{ NbtValue(std::int32_t{ 1 }), NbtValue(std::string("x")) };
    EXPECT_TRUE(encode_tree(root, Layout::Unnamed).empty());
}

TEST(TagCodec, RefusesAValueWiderThanItsLengthField) {
    NbtCompound root;
    root["long"] = std::string(70000, 'x');
    EXPECT_TRUE(encode_tree(root, Layout::Unnamed).empty());
}

TEST(TagCodec, NeverNarrowsAnUnsignedIntSilently) {
    static_assert(!std::is_constructible_v<NbtValue, unsigned int>);
    const NbtValue value(300);
    const auto* integer = value.get_if<std::int32_t>();
    ASSERT_NE(integer, nullptr);
    EXPECT_EQ(*integer, 300);
}

TEST(TagCodec, RefusesNestingDeeperThanTheBudget) {
    std::vector<std::uint8_t> bytes{ 0x0A };
    for (int i = 0; i < 512; ++i) {
        bytes.push_back(0x0A);
        bytes.push_back(0x00);
        bytes.push_back(0x00);
    }
    bytes.insert(bytes.end(), 513, std::uint8_t{ 0 });
    NbtReader reader(bytes);
    EXPECT_FALSE(reader.root_map().has_value());
}

TEST(TagCodec, RefusesAPayloadOverItsByteBudget) {
    const std::size_t count = 1'100'000;
    std::vector<std::uint8_t> bytes{ 0x09, 0x01 };
    bytes.push_back(static_cast<std::uint8_t>((count >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((count >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((count >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(count & 0xFF));
    bytes.resize(bytes.size() + count, 0x00);

    // One byte per entry fits the default allowance, so the test asks for a small one.
    const ReadBudget tight(4096, 16);
    NbtReader reader(bytes, Layout::Unnamed, tight);
    EXPECT_FALSE(reader.root().has_value());
}

TEST(TagCodec, ReadsAHomogeneousListBack) {
    const NbtList values{ NbtValue(std::int32_t{ 1 }), NbtValue(std::int32_t{ 2 }),
        NbtValue(std::int32_t{ 3 }) };
    NbtCompound root;
    root["values"] = values;
    const auto bytes = encode_tree(root, Layout::Unnamed);
    NbtReader reader(bytes);
    auto parsed = reader.root_map();
    ASSERT_TRUE(parsed.has_value());
    const auto* list = parsed->at("values").get_if<NbtList>();
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(*list, values);
}

TEST(TagText, WritesCanonicalText) {
    NbtCompound root;
    root["text"] = std::string("Big Sword");
    root["count"] = std::int32_t{ 3 };
    root["ratio"] = 1.5f;
    root["bytes"] = NbtBytes{ 1, 2 };
    root["list"] = NbtList{ NbtValue(std::int32_t{ 1 }), NbtValue(std::int32_t{ 2 }) };
    // A map keeps its entries sorted by name, which is what the binary form writes too.
    EXPECT_EQ(to_snbt(NbtValue(root)),
        "{bytes:[B;1B,2B],count:3,list:[1,2],ratio:1.5f,text:\"Big Sword\"}");
}

TEST(TagText, QuotesNamesThatWouldReadBackAsSomethingElse) {
    NbtCompound root;
    root["true"] = std::int32_t{ 1 };
    root["plain.name"] = std::int32_t{ 2 };
    EXPECT_EQ(to_snbt(NbtValue(root)), "{plain.name:2,\"true\":1}");
}

TEST(TagText, PrintsAnIndentedDump) {
    NbtCompound root;
    root["child"] = NbtCompound{ { "leaf", std::int32_t{ 9 } } };
    EXPECT_EQ(to_snbt_pretty(NbtValue(root)), "{\n  child: {\n    leaf: 9\n  }\n}");
}

TEST(TagText, DescribesTheShapeOfATree) {
    NbtCompound root;
    root["child"] = NbtCompound{ { "leaf", std::int32_t{ 9 } } };
    const auto lines = describe_tree(NbtValue(root));
    ASSERT_EQ(lines.size(), 2);
    EXPECT_EQ(lines[0], "child: map");
    EXPECT_EQ(lines[1], "  leaf: int");
}

TEST(TagText, ParsesTextBackIntoTheSameTree) {
    const auto parsed = parse_snbt("{levels:{\"minecraft:sharpness\":5}}");
    ASSERT_TRUE(parsed.has_value());
    const auto* entries = at(*parsed, "levels");
    ASSERT_NE(entries, nullptr);
    EXPECT_EQ(int_at(*entries, "minecraft:sharpness"), 5);
    EXPECT_EQ(to_snbt(*parsed), "{levels:{\"minecraft:sharpness\":5}}");
}

TEST(TagText, KeepsNumbersInTheirTypedForm) {
    EXPECT_EQ(parse_snbt("5")->tag(), Tag::Int);
    EXPECT_EQ(parse_snbt("5b")->tag(), Tag::Byte);
    EXPECT_EQ(parse_snbt("5s")->tag(), Tag::Short);
    EXPECT_EQ(parse_snbt("5L")->tag(), Tag::Long);
    EXPECT_EQ(parse_snbt("1.5")->tag(), Tag::Double);
    EXPECT_EQ(parse_snbt("1.5f")->tag(), Tag::Float);
    EXPECT_EQ(parse_snbt("true")->tag(), Tag::Byte);
    // Too wide for an int, so it becomes a long the way the game does it.
    EXPECT_EQ(parse_snbt("3000000000")->tag(), Tag::Long);
}

TEST(TagText, ReadsQuotedStringsAndBareWords) {
    const auto quoted = parse_snbt("\"Big Sword\"");
    ASSERT_TRUE(quoted.has_value());
    ASSERT_NE(quoted->get_if<std::string>(), nullptr);
    EXPECT_EQ(*quoted->get_if<std::string>(), "Big Sword");

    const auto bare = parse_snbt("minecraft:efficiency");
    ASSERT_TRUE(bare.has_value());
    ASSERT_NE(bare->get_if<std::string>(), nullptr);
    EXPECT_EQ(*bare->get_if<std::string>(), "minecraft:efficiency");
}

TEST(TagText, ReadsListsAndTypedArrays) {
    const auto list = parse_snbt("[1,2,3]");
    ASSERT_TRUE(list.has_value());
    ASSERT_NE(list->get_if<NbtList>(), nullptr);
    EXPECT_EQ(list->get_if<NbtList>()->size(), 3);

    const auto ints = parse_snbt("[I;1,2]");
    ASSERT_TRUE(ints.has_value());
    ASSERT_NE(ints->get_if<NbtInts>(), nullptr);
    EXPECT_EQ(ints->get_if<NbtInts>()->size(), 2);

    const auto bytes = parse_snbt("[B;1b,2b]");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->tag(), Tag::Bytes);
}

TEST(TagText, RejectsTrailingAndUnfinishedText) {
    EXPECT_FALSE(parse_snbt("{a:1} extra").has_value());
    EXPECT_FALSE(parse_snbt("{a:1").has_value());
    EXPECT_FALSE(parse_snbt("[1,\"two\"]").has_value());   // one element type per list
}

TEST(TagText, RejectsNestingDeeperThanTheGrammarAllows) {
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text += "{a:";
    }
    text += "1";
    for (int i = 0; i < 200; ++i) {
        text += "}";
    }
    EXPECT_FALSE(parse_snbt(text).has_value());
}

// ---- limits reported by the audit -----------------------------------------------------

TEST(TagBudget, ChargesAListForTheNodesItAllocates) {
    // 4000 single-byte elements: 4 KB on the wire, but every one of them becomes a variant
    // in memory. A budget that only counted wire bytes would let a small payload ask for
    // hundreds of kilobytes.
    std::vector<std::uint8_t> bytes{ 0x07, 0x01 };   // list of bytes
    const std::size_t count = 4000;
    bytes.push_back(static_cast<std::uint8_t>((count >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((count >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((count >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(count & 0xFF));
    bytes.resize(bytes.size() + count, 0x00);

    // Generous for the bytes on the wire, far too small for 4000 nodes.
    const ReadBudget budget(64 * 1024, 16);
    NbtReader reader(bytes, Layout::Unnamed, budget);
    EXPECT_FALSE(reader.root().has_value());
}

TEST(TagText, JoinsSurrogatePairsIntoOneCharacter) {
    // Raw strings keep the escapes as text, which is what the parser has to read.
    const auto value = parse_snbt(R"("\uD83D\uDE00")");
    ASSERT_TRUE(value.has_value());
    const auto* text = value->get_if<std::string>();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(*text, std::string("\xF0\x9F\x98\x80"));
}

TEST(TagText, RejectsLoneSurrogates) {
    EXPECT_FALSE(parse_snbt(R"("\uD83D")").has_value());
    EXPECT_FALSE(parse_snbt(R"("\uDE00")").has_value());
}

TEST(TagText, WritesAndReadsNaNAndInfinity) {
    const auto nan = parse_snbt(to_snbt(NbtValue(std::numeric_limits<double>::quiet_NaN())));
    ASSERT_TRUE(nan.has_value());
    const auto* nan_value = nan->get_if<double>();
    ASSERT_NE(nan_value, nullptr);
    EXPECT_TRUE(std::isnan(*nan_value));

    const auto infinity = parse_snbt(to_snbt(NbtValue(-std::numeric_limits<double>::infinity())));
    ASSERT_TRUE(infinity.has_value());
    const auto* infinity_value = infinity->get_if<double>();
    ASSERT_NE(infinity_value, nullptr);
    EXPECT_TRUE(std::isinf(*infinity_value));
    EXPECT_LT(*infinity_value, 0.0);
}

TEST(TagText, KeepsByteArraysSigned) {
    const auto value = parse_snbt("[B;-56B,7B]");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(to_snbt(*value), "[B;-56B,7B]");

    // 200 is not a byte; the writer used to print it as one, which read back as -56.
    EXPECT_FALSE(parse_snbt("[B;200B]").has_value());
}

TEST(TagText, RejectsNumbersThatDoNotFit) {
    EXPECT_FALSE(parse_snbt("99999999999999999999").has_value());
    EXPECT_FALSE(parse_snbt("99999999999999999999L").has_value());
    EXPECT_FALSE(parse_snbt("200b").has_value());
}
