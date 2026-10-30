#include <gtest/gtest.h>
#include "protocol/varint.hpp"

TEST(VarIntTest, Roundtrip) {
    const std::int32_t values[] = { 0, 1, 127, 128, 300, -1, INT32_MAX, INT32_MIN };

    for (std::int32_t value : values) {
        std::vector<std::uint8_t> buffer;
        terila::protocol::write_varint(buffer, value);
        auto result = terila::protocol::read_varint(buffer.data(), buffer.size());

        ASSERT_TRUE(result.has_value()) << "value: " << value;
        EXPECT_EQ(result->value, value);
        EXPECT_EQ(result->consumed, buffer.size());
    }
}

TEST(VarIntTest, EncodesKnownValues) {
    std::vector<std::uint8_t> buffer;
    terila::protocol::write_varint(buffer, 300);
    EXPECT_EQ(buffer.size(), 2);
    EXPECT_EQ(buffer[0], 0xAC);
    EXPECT_EQ(buffer[1], 0x02);
}

TEST(VarIntTest, RejectsInvalidInput) {
    EXPECT_FALSE(terila::protocol::read_varint(nullptr, 0).has_value());

    std::uint8_t data1[] = { 0x01 };
    EXPECT_FALSE(terila::protocol::read_varint(data1, 0).has_value());

    std::uint8_t data2[] = { 0x80 };
    EXPECT_FALSE(terila::protocol::read_varint(data2, 1).has_value());

    std::uint8_t data3[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };
    EXPECT_FALSE(terila::protocol::read_varint(data3, 6).has_value());
}

TEST(VarIntTest, StopsAtFirstVarInt) {

    std::uint8_t data[] = { 0x05, 0xFF, 0xFF };
    auto result = terila::protocol::read_varint(data, 3);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value, 5);
    EXPECT_EQ(result->consumed, 1);

}