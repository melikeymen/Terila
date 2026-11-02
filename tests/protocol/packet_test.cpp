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

TEST(PacketTest, RoundTrip) {
    std::vector<std::uint8_t> payload = { 0xAA, 0xBB, 0xCC };
    std::vector<std::uint8_t> buffer;

    EXPECT_TRUE(terila::protocol::write_packet(buffer, 0x05, payload));

    auto result = terila::protocol::read_packet(buffer.data(), buffer.size());
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->id, 0x05);
    EXPECT_EQ(result->payload, payload);
    EXPECT_EQ(result->consumed, buffer.size());
}

TEST(PacketTest, ReadsPrefix) {
    std::vector<std::uint8_t> buffer;

    EXPECT_TRUE(terila::protocol::write_packet(buffer, 0x01, { 'x' }));
    EXPECT_TRUE(terila::protocol::write_packet(buffer, 0x02, { 'y' }));

    auto r1 = terila::protocol::read_packet(buffer.data(), buffer.size());
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->id, 0x01);
    EXPECT_EQ(r1->payload, std::vector<std::uint8_t>{ 'x' });

    auto r2 = terila::protocol::read_packet(buffer.data() + r1->consumed, buffer.size() - r1->consumed);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->id, 0x02);
    EXPECT_EQ(r2->payload, std::vector<std::uint8_t>{ 'y' });
}