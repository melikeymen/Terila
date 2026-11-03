#include <gtest/gtest.h>
#include "protocol/status.hpp"

TEST(StatusTest, SmokeTest) {
    std::vector<std::uint8_t> buffer;
    terila::protocol::write_status_request(buffer);
    EXPECT_TRUE(terila::protocol::read_status_request(buffer.data(), buffer.size()));
}