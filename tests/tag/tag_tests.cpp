#include "tag/tag.hpp"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <vector>

using namespace terila::tag;

namespace {

    TagMap sample_tree() {
        TagMap root;
        root["byte"] = std::int8_t{ -3 };
        root["short"] = std::int16_t{ 32000 };
        root["int"] = std::int32_t{ 123456 };
        root["long"] = std::int64_t{ 9876543210LL };
        root["float"] = 1.25f;
        root["double"] = 3.5;
        root["text"] = std::string("hello");
        root["bytes"] = TagBytes{ 1, 254 };
        root["ints"] = TagInts{ 1, -2 };
        root["longs"] = TagLongs{ 3, -4 };
        root["child"] = TagMap{ { "middle", TagMap{ { "leaf", std::int32_t{ 9 } } } } };
        root["list"] = TagList{
            TagValue(TagMap{ { "v", std::int32_t{ 1 } } }),
            TagValue(TagMap{ { "v", std::int32_t{ 2 } } })
        };
        return root;
    }

    std::vector<std::uint8_t> encode_tree(const TagMap& root, Layout layout) {
        TagWriter writer(layout);
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
        TagReader reader(bytes, layout);
        auto parsed = reader.root_map();
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, root);
        EXPECT_EQ(reader.position(), bytes.size());
    }
}

TEST(TagCodec, KeepsEmptyContainers) {
    const TagMap root{ { "empty", TagMap{} }, { "list", TagList{} } };
    const auto bytes = encode_tree(root, Layout::Unnamed);
    TagReader reader(bytes);
    auto parsed = reader.root_map();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, root);
}

TEST(TagCodec, RefusesTruncatedAndUnknownPayloads) {
    const std::vector<std::uint8_t> truncated{ 10, 8, 0, 1 };
    TagReader first(truncated);
    EXPECT_FALSE(first.root_map().has_value());

    const std::vector<std::uint8_t> unknown_kind{ 10, 99 };
    TagReader second(unknown_kind);
    EXPECT_FALSE(second.root_map().has_value());

    const std::vector<std::uint8_t> huge_text{ 10, 8, 0, 0xff, 0xff, 0xff, 0xff, 0x07 };
    TagReader third(huge_text);
    EXPECT_FALSE(third.root_map().has_value());
}

TEST(TagCodec, WritesAChatComponentAsAnUnnamedValue) {
    const auto bytes = text_component_bytes("hello");
    EXPECT_EQ(bytes, (std::vector<std::uint8_t>{
        10, 8, 0, 4, 't', 'e', 'x', 't', 0, 5, 'h', 'e', 'l', 'l', 'o', 0 }));
}

TEST(TagCodec, RefusesAListThatMixesTypes) {
    TagMap root;
    root["mixed"] = TagList{ TagValue(std::int32_t{ 1 }), TagValue(std::string("x")) };
    EXPECT_TRUE(encode_tree(root, Layout::Unnamed).empty());
}

TEST(TagCodec, RefusesAValueWiderThanItsLengthField) {
    TagMap root;
    root["long"] = std::string(70000, 'x');
    EXPECT_TRUE(encode_tree(root, Layout::Unnamed).empty());
}

TEST(TagCodec, NeverNarrowsAnUnsignedIntSilently) {
    static_assert(!std::is_constructible_v<TagValue, unsigned int>);
    const TagValue value(300);
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
    TagReader reader(bytes);
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
    TagReader reader(bytes, Layout::Unnamed, tight);
    EXPECT_FALSE(reader.root().has_value());
}

TEST(TagCodec, ReadsAHomogeneousListBack) {
    const TagList values{ TagValue(std::int32_t{ 1 }), TagValue(std::int32_t{ 2 }),
        TagValue(std::int32_t{ 3 }) };
    TagMap root;
    root["values"] = values;
    const auto bytes = encode_tree(root, Layout::Unnamed);
    TagReader reader(bytes);
    auto parsed = reader.root_map();
    ASSERT_TRUE(parsed.has_value());
    const auto* list = parsed->at("values").get_if<TagList>();
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(*list, values);
}

TEST(TagText, WritesCanonicalText) {
    TagMap root;
    root["text"] = std::string("Big Sword");
    root["count"] = std::int32_t{ 3 };
    root["ratio"] = 1.5f;
    root["bytes"] = TagBytes{ 1, 2 };
    root["list"] = TagList{ TagValue(std::int32_t{ 1 }), TagValue(std::int32_t{ 2 }) };
    // A map keeps its entries sorted by name, which is what the binary form writes too.
    EXPECT_EQ(to_text(TagValue(root)),
        "{bytes:[B;1B, 2B],count:3,list:[1, 2],ratio:1.5f,text:\"Big Sword\"}");
}

TEST(TagText, QuotesNamesThatWouldReadBackAsSomethingElse) {
    TagMap root;
    root["true"] = std::int32_t{ 1 };
    root["plain.name"] = std::int32_t{ 2 };
    EXPECT_EQ(to_text(TagValue(root)), "{plain.name:2,\"true\":1}");
}

TEST(TagText, PrintsAnIndentedDump) {
    TagMap root;
    root["child"] = TagMap{ { "leaf", std::int32_t{ 9 } } };
    EXPECT_EQ(to_pretty(TagValue(root)), "{\n  child: {\n    leaf: 9\n  }\n}");
}

TEST(TagText, DescribesTheShapeOfATree) {
    TagMap root;
    root["child"] = TagMap{ { "leaf", std::int32_t{ 9 } } };
    const auto lines = describe_tree(TagValue(root));
    ASSERT_EQ(lines.size(), 2);
    EXPECT_EQ(lines[0], "child: map");
    EXPECT_EQ(lines[1], "  leaf: int");
}

TEST(TagText, ParsesTextBackIntoTheSameTree) {
    const auto parsed = parse_tag("{levels:{\"minecraft:sharpness\":5}}");
    ASSERT_TRUE(parsed.has_value());
    const auto* entries = at(*parsed, "levels");
    ASSERT_NE(entries, nullptr);
    EXPECT_EQ(int_at(*entries, "minecraft:sharpness"), 5);
    EXPECT_EQ(to_text(*parsed), "{levels:{\"minecraft:sharpness\":5}}");
}

TEST(TagText, KeepsNumbersInTheirTypedForm) {
    EXPECT_EQ(parse_tag("5")->tag(), TagKind::Int);
    EXPECT_EQ(parse_tag("5b")->tag(), TagKind::Byte);
    EXPECT_EQ(parse_tag("5s")->tag(), TagKind::Short);
    EXPECT_EQ(parse_tag("5L")->tag(), TagKind::Long);
    EXPECT_EQ(parse_tag("1.5")->tag(), TagKind::Double);
    EXPECT_EQ(parse_tag("1.5f")->tag(), TagKind::Float);
    EXPECT_EQ(parse_tag("true")->tag(), TagKind::Byte);
    // Too wide for an int, so it becomes a long the way the game does it.
    EXPECT_EQ(parse_tag("3000000000")->tag(), TagKind::Long);
}

TEST(TagText, ReadsQuotedStringsAndBareWords) {
    const auto quoted = parse_tag("\"Big Sword\"");
    ASSERT_TRUE(quoted.has_value());
    ASSERT_NE(quoted->get_if<std::string>(), nullptr);
    EXPECT_EQ(*quoted->get_if<std::string>(), "Big Sword");

    const auto bare = parse_tag("minecraft:efficiency");
    ASSERT_TRUE(bare.has_value());
    ASSERT_NE(bare->get_if<std::string>(), nullptr);
    EXPECT_EQ(*bare->get_if<std::string>(), "minecraft:efficiency");
}

TEST(TagText, ReadsListsAndTypedArrays) {
    const auto list = parse_tag("[1,2,3]");
    ASSERT_TRUE(list.has_value());
    ASSERT_NE(list->get_if<TagList>(), nullptr);
    EXPECT_EQ(list->get_if<TagList>()->size(), 3);

    const auto ints = parse_tag("[I;1,2]");
    ASSERT_TRUE(ints.has_value());
    ASSERT_NE(ints->get_if<TagInts>(), nullptr);
    EXPECT_EQ(ints->get_if<TagInts>()->size(), 2);

    const auto bytes = parse_tag("[B;1b,2b]");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->tag(), TagKind::Bytes);
}

TEST(TagText, RejectsTrailingAndUnfinishedText) {
    EXPECT_FALSE(parse_tag("{a:1} extra").has_value());
    EXPECT_FALSE(parse_tag("{a:1").has_value());
    EXPECT_FALSE(parse_tag("[1,\"two\"]").has_value());   // one element type per list
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
    EXPECT_FALSE(parse_tag(text).has_value());
}
