#include "protocol/chat.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

TEST(ChatTest, SystemChatEncodesTextComponent) {
    std::vector<std::uint8_t> out;
    terila::protocol::packet_ids().play_system_chat = 0x7C;
    ASSERT_TRUE(terila::protocol::write_system_chat(out, "Hello"));

    ASSERT_EQ(out.size(), 19u);
    EXPECT_EQ(out[2], 0x0A);
    EXPECT_EQ(out[3], 0x08);
    EXPECT_EQ(out[4], 0x00);
    EXPECT_EQ(out[5], 0x04);
    EXPECT_EQ(out[6], 't');
    EXPECT_EQ(out[7], 'e');
    EXPECT_EQ(out[8], 'x');
    EXPECT_EQ(out[9], 't');
    EXPECT_EQ(out[10], 0x00);
    EXPECT_EQ(out[11], 0x05);
    EXPECT_EQ(out[12], 'H');
    EXPECT_EQ(out[13], 'e');
    EXPECT_EQ(out[14], 'l');
    EXPECT_EQ(out[15], 'l');
    EXPECT_EQ(out[16], 'o');
    EXPECT_EQ(out[17], 0x00);
    EXPECT_EQ(out[18], 0x00);
}
