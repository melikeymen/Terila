#include <gtest/gtest.h>
#include "protocol/string.hpp"

TEST(StringTest, WritesShortAscii) {
    std::vector<std::uint8_t> buffer;
    EXPECT_TRUE(terila::protocol::write_string(buffer, "abc"));
    EXPECT_EQ(buffer.size(), 4);
    EXPECT_EQ(buffer[0], 0x03);
    EXPECT_EQ(buffer[1], 'a');
    EXPECT_EQ(buffer[2], 'b');
    EXPECT_EQ(buffer[3], 'c');
}

TEST(StringTest, WritesEmpty) {
    std::vector<std::uint8_t> buffer;
    EXPECT_TRUE(terila::protocol::write_string(buffer, ""));
    EXPECT_EQ(buffer.size(), 1);
    EXPECT_EQ(buffer[0], 0x00);
}

TEST(StringTest, WritesUtf8) {
    std::vector<std::uint8_t> buffer;
    EXPECT_TRUE(terila::protocol::write_string(buffer, "ş"));
    EXPECT_EQ(buffer.size(), 3);
    EXPECT_EQ(buffer[0], 0x02);
    EXPECT_EQ(buffer[1], 0xC5);
    EXPECT_EQ(buffer[2], 0x9F);
}

TEST(StringTest, RejectsTooLong) {
    std::vector<std::uint8_t> buffer;
    std::string too_long(32768, 'a');
    EXPECT_FALSE(terila::protocol::write_string(buffer, too_long));
    EXPECT_TRUE(buffer.empty());
}

TEST(StringTest, AppendsToExistingBuffer) {
    std::vector<std::uint8_t> buffer = { 0xFF };
    EXPECT_TRUE(terila::protocol::write_string(buffer, "ab"));
    EXPECT_EQ(buffer.size(), 4);
    EXPECT_EQ(buffer[0], 0xFF);
    EXPECT_EQ(buffer[1], 0x02);
    EXPECT_EQ(buffer[2], 'a');
    EXPECT_EQ(buffer[3], 'b');
}

TEST(StringTest, RoundTrip) {
    const std::string values[] = { "A", "b", "Ş", "ĞA", "ÇÇ", "Ğü", "üĞ" };

    for (const std::string& value : values) {

        std::vector<std::uint8_t> buffer;
        EXPECT_TRUE(terila::protocol::write_string(buffer, value));

        auto result = terila::protocol::read_string(buffer.data(), buffer.size());
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), value);

    }
}

TEST(StringTest, RejectsInvalidInput) {
    EXPECT_FALSE(terila::protocol::read_string(nullptr, 0).has_value());

    std::uint8_t data1[] = { 0x01 };
    EXPECT_FALSE(terila::protocol::read_string(data1, 0).has_value());

    std::uint8_t data2[] = { 0x80 };
    EXPECT_FALSE(terila::protocol::read_string(data2, 1).has_value());

    std::uint8_t data3[] = { 0x05, 'a', 'b' };
    EXPECT_FALSE(terila::protocol::read_string(data3, 3).has_value());

    std::uint8_t data4[] = { 0x80, 0x80, 0x02 };
    EXPECT_FALSE(terila::protocol::read_string(data4, 3).has_value());
}