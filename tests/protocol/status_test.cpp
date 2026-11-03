#include <gtest/gtest.h>
#include "protocol/status.hpp"

TEST(StatusTest, SmokeTest) {
    std::vector<std::uint8_t> buffer;
    terila::protocol::write_status_request(buffer);
    EXPECT_TRUE(terila::protocol::read_status_request(buffer.data(), buffer.size()));
}

TEST(StatusTest, PingRoundTrip) {
    std::vector<std::uint8_t> buffer;
    terila::protocol::write_ping(buffer, 1234567890);
    auto result = terila::protocol::read_ping(buffer.data(), buffer.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1234567890);
}

TEST(StatusTest, PingRejectsWrongId) {
    std::vector<std::uint8_t> buffer;
    std::vector<std::uint8_t> payload = { 0x01, 0x02 };
    terila::protocol::write_packet(buffer, 0x00, payload);   // yanlış ID (Status, Ping değil)

    EXPECT_FALSE(terila::protocol::read_ping(buffer.data(), buffer.size()).has_value());
}