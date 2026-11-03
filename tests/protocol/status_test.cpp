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

TEST(StatusTest, PongRoundTrip) {

    std::vector<std::uint8_t> buffer;

    terila::protocol::write_pong(buffer, -1);
    auto result = terila::protocol::read_pong(buffer.data(), buffer.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), -1);
}

TEST(StatusTest, PongRejectsWrongId) {

    std::vector<std::uint8_t> buffer;

    std::vector<std::uint8_t> payload = { 0x01, 0x02 };
    terila::protocol::write_packet(buffer, 0x00, payload);

    EXPECT_FALSE(terila::protocol::read_pong(buffer.data(), buffer.size()).has_value());
}

TEST(StatusTest, StatusResponseRoundTrip) {

    terila::protocol::StatusResponse response;
    response.version_name = "26.3";
    response.version_protocol = 777;
    response.players_max = 20;
    response.players_online = 0;
    response.description = "Terila Server";
    response.favicon = "data:image/png;base64,ABC";

    std::vector<std::uint8_t> buffer;

    EXPECT_TRUE(terila::protocol::write_status_response(buffer, response));
    auto result = terila::protocol::read_status_response(buffer.data(), buffer.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->version_name, "26.3");
    EXPECT_EQ(result->version_protocol, 777);
    EXPECT_EQ(result->players_max, 20);
    EXPECT_EQ(result->players_online, 0);
    EXPECT_EQ(result->description, "Terila Server");
    EXPECT_EQ(result->favicon, "data:image/png;base64,ABC");
}