#include <gtest/gtest.h>

#include "nbt/nbt_reader.hpp"

#include <cstdint>
#include <vector>

namespace nbt = terila::nbt;

TEST(NbtReaderTest, ReadsAnEmptyRootCompound) {
    // Network NBT: the root compound carries no name, only its payload.
    const std::vector<std::uint8_t> bytes{ 0x0A, 0x00 };
    nbt::Reader reader(bytes);
    EXPECT_TRUE(reader.read_root_compound().has_value());
}

TEST(NbtReaderTest, ReadsASmallList) {
    const std::vector<std::uint8_t> bytes{
        0x02,                    // element type: Short
        0x00, 0x00, 0x00, 0x02,  // count
        0x00, 0x01, 0x00, 0x02   // two shorts
    };
    nbt::Reader reader(bytes);
    EXPECT_TRUE(reader.read_payload(nbt::Tag::List).has_value());
}

// One input byte can create a whole NbtValue, so a count larger than the bytes that
// remain must be rejected before anything is reserved.
TEST(NbtReaderTest, RejectsAListCountLargerThanTheInput) {
    const std::vector<std::uint8_t> bytes{
        0x01,                    // element type: Byte
        0xFF, 0xFF, 0xFF, 0x7F,  // count: 268435455, input holds three bytes
        0x00, 0x00, 0x00
    };
    nbt::Reader reader(bytes);
    EXPECT_FALSE(reader.read_payload(nbt::Tag::List).has_value());
}

// The count is legal for its input size (one byte per element), but the total node
// budget still has to stop it: 1.1M values would be tens of megabytes.
TEST(NbtReaderTest, StopsAtTheNodeBudget) {
    const std::size_t count = 1'100'000;
    std::vector<std::uint8_t> bytes;
    bytes.push_back(0x01);   // element type: Byte
    bytes.push_back(static_cast<std::uint8_t>((count >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((count >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((count >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(count & 0xFF));
    bytes.resize(5 + count, 0x00);

    nbt::Reader reader(bytes);
    EXPECT_FALSE(reader.read_payload(nbt::Tag::List).has_value());
}

TEST(NbtReaderTest, RejectsATruncatedArray) {
    const std::vector<std::uint8_t> bytes{
        0x00, 0x00, 0x00, 0x04,  // count: 4 ints
        0x00, 0x00, 0x00, 0x01   // only one int follows
    };
    nbt::Reader reader(bytes);
    EXPECT_FALSE(reader.read_payload(nbt::Tag::IntArray).has_value());
}
