#include <gtest/gtest.h>
#include "protocol/handshake.hpp"

TEST(HandshakeTest, WriteAndRead) {

	std::vector<std::uint8_t> buffer;

	EXPECT_TRUE(terila::protocol::write_handshake(buffer, 777, "192.168.1.100", 25565, 2));
	auto result = terila::protocol::read_handshake(buffer.data(), buffer.size());
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->protocol, 777);
	EXPECT_EQ(result->address, "192.168.1.100");
	EXPECT_EQ(result->port, 25565);
	EXPECT_EQ(result->next_state, 2);

}

TEST(HandshakeTest, RejectsWrongId) {

	std::vector<std::uint8_t> buffer;
	std::vector<std::uint8_t> payload = { 0x01, 0x02 };
	terila::protocol::write_packet(buffer, 0x01, payload); // its not handshake

	auto result = terila::protocol::read_handshake(buffer.data(), buffer.size());
	EXPECT_FALSE(result.has_value());

	EXPECT_FALSE(terila::protocol::read_handshake(nullptr, 0).has_value());

	std::uint8_t data[] = { 0x05, 0x00, 0x01 };
	EXPECT_FALSE(terila::protocol::read_handshake(data, 3).has_value());
}

TEST(HandshakeTest, RejectsInvalidInput) {

	EXPECT_FALSE(terila::protocol::read_handshake(nullptr, 0).has_value());

	std::uint8_t data[] = { 0x05, 0x00, 0x01 };
	EXPECT_FALSE(terila::protocol::read_handshake(data, 3).has_value());
}
