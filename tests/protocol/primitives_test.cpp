#include <gtest/gtest.h>
#include "protocol/primitives.hpp"

TEST(Primitives, Uint16Roundtrip) {

	std::vector<std::uint8_t> buffer;

	terila::protocol::write_uint16(buffer, 25565);
	auto result = terila::protocol::read_uint16(buffer.data(), buffer.size());
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result.value(), 25565);
	EXPECT_EQ(buffer.size(), 2);

}

TEST(Primitives, Int64Roundtrip) {
	std::vector<std::uint8_t> buffer;

	terila::protocol::write_int64(buffer, 100'000);
	auto result = terila::protocol::read_int64(buffer.data(), buffer.size());
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result.value(), 100'000);
	EXPECT_EQ(buffer.size(), 8);
}