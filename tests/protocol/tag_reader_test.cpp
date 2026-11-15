#include <gtest/gtest.h>

#include "tag/codec.hpp"
#include "tag/text.hpp"

#include <cstdint>
#include <vector>

namespace tag = terila::tag;

namespace {

    // One compound child per level, each with a three byte name.
    std::vector<std::uint8_t> nested_compounds(std::size_t levels) {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(levels * 6 + 1 + levels);
        bytes.push_back(0x0A);   // the root map
        for (std::size_t i = 0; i < levels; ++i) {
            bytes.push_back(0x0A);            // a map child
            bytes.push_back(0x00);            // name length: 3
            bytes.push_back(0x03);
            bytes.push_back(static_cast<std::uint8_t>((i >> 16) & 0xFF));
            bytes.push_back(static_cast<std::uint8_t>((i >> 8) & 0xFF));
            bytes.push_back(static_cast<std::uint8_t>(i & 0xFF));
        }
        // One end marker per open map: the innermost first, then the root.
        bytes.insert(bytes.end(), levels + 1, 0x00);
        return bytes;
    }

} // namespace

TEST(TagReaderTest, ReadsAnEmptyRootMap) {
    const std::vector<std::uint8_t> bytes{ 0x0A, 0x00 };
    tag::TagReader reader(bytes);
    EXPECT_TRUE(reader.root_map().has_value());
}

TEST(TagReaderTest, ReadsASmallList) {
    const std::vector<std::uint8_t> bytes{
        0x09,                    // the value is a list
        0x02,                    // element type: short
        0x00, 0x00, 0x00, 0x02,  // count
        0x00, 0x01, 0x00, 0x02   // two shorts
    };
    tag::TagReader reader(bytes);
    EXPECT_TRUE(reader.root().has_value());
}

// One count field can claim far more than the payload holds, so it has to be checked
// before anything is reserved.
TEST(TagReaderTest, RejectsAListCountLargerThanTheInput) {
    const std::vector<std::uint8_t> bytes{
        0x09,
        0x01,                    // element type: byte
        0xFF, 0xFF, 0xFF, 0x7F,  // count: 268435455, input holds three bytes
        0x00, 0x00, 0x00
    };
    tag::TagReader reader(bytes);
    EXPECT_FALSE(reader.root().has_value());
}

// The count is legal for its input size, but the byte budget still has to stop it.
TEST(TagReaderTest, StopsAtTheByteBudget) {
    const std::size_t count = 1'100'000;
    std::vector<std::uint8_t> bytes{ 0x09, 0x01 };
    bytes.push_back(static_cast<std::uint8_t>((count >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((count >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((count >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(count & 0xFF));
    bytes.resize(bytes.size() + count, 0x00);

    const tag::ReadBudget tight(4096, 16);
    tag::TagReader reader(bytes, tag::Layout::Unnamed, tight);
    EXPECT_FALSE(reader.root().has_value());
}

// Nesting is recursion: without a bound a payload of map headers walks the C++ stack
// until it overflows, which is what the fuzzer hit.
TEST(TagReaderTest, RejectsDeepNestingInsteadOfOverflowingTheStack) {
    const auto bytes = nested_compounds(100'000);
    tag::TagReader reader(bytes);
    EXPECT_FALSE(reader.root_map().has_value());
}

TEST(TagReaderTest, ReadsNestingUpToTheLimit) {
    const auto bytes = nested_compounds(40);
    tag::TagReader reader(bytes);
    EXPECT_TRUE(reader.root_map().has_value());
}

TEST(TagReaderTest, RejectsATruncatedArray) {
    const std::vector<std::uint8_t> bytes{
        0x0B,                    // int array
        0x00, 0x00, 0x00, 0x04,  // count: 4
        0x00, 0x00, 0x00, 0x01   // only one int follows
    };
    tag::TagReader reader(bytes);
    EXPECT_FALSE(reader.root().has_value());
}
