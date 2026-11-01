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