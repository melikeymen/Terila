#include <gtest/gtest.h>
#include "protocol/packet.hpp"

TEST(PacketTest, WritesSimple) {
    std::vector<std::uint8_t> payload = { 0x01, 0x02 };
    std::vector<std::uint8_t> buffer;
    EXPECT_TRUE(terila::protocol::write_packet(buffer, 0x00, payload));
    EXPECT_EQ(buffer.size(), 4);
    EXPECT_EQ(buffer[0], 0x03);
    EXPECT_EQ(buffer[1], 0x00);
    EXPECT_EQ(buffer[2], 0x01);
    EXPECT_EQ(buffer[3], 0x02);
}

TEST(PacketTest, RejectsInvalidInputWrite) {
    std::vector<std::uint8_t> payload = { 0x01, 0x02 };
    std::vector<std::uint8_t> buffer;
    EXPECT_FALSE(terila::protocol::write_packet(buffer, -1, payload));
}