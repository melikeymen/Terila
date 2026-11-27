#include <gtest/gtest.h>
#include "nbt/nbt_text.hpp"
#include "protocol/disconnect.hpp"
#include "protocol/login.hpp"
#include "protocol/packet.hpp"
#include "protocol/varint.hpp"
#include "protocol/string.hpp"
#include <array>
#include <vector>
#include <string>

using namespace terila::protocol;

static std::vector<std::uint8_t> make_login_start_packet(const std::string& username) {
    std::vector<std::uint8_t> payload;
    write_string(payload, username);

    std::vector<std::uint8_t> out;
    write_packet(out, 0x00, payload);
    return out;
}


TEST(LoginTest, ReadLoginStartValid) {
    auto packet = make_login_start_packet("Steve");

    auto result = read_login_start(packet.data(), packet.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->username, "Steve");
}

static std::vector<std::uint8_t> make_login_start_packet_with_uuid(
    const std::string& username, const std::array<std::uint8_t, 16>& uuid) {
    std::vector<std::uint8_t> payload;
    write_string(payload, username);
    payload.insert(payload.end(), uuid.begin(), uuid.end());

    std::vector<std::uint8_t> out;
    write_packet(out, 0x00, payload);
    return out;
}

// A premium launcher puts the account's real UUID in the hello packet, which is what lets
// the server skip the Mojang round trip on login.
TEST(LoginTest, ReadLoginStartCarriesTheClaimedProfile) {
    const std::array<std::uint8_t, 16> uuid{ 0x06, 0x9a, 0x79, 0xf4, 0x44, 0xe9, 0x47, 0x26,
        0xa5, 0xbe, 0xfc, 0xa9, 0x0e, 0x38, 0xaa, 0xf5 };
    auto packet = make_login_start_packet_with_uuid("Notch", uuid);

    auto result = read_login_start(packet.data(), packet.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->username, "Notch");
    EXPECT_TRUE(result->has_uuid);
    EXPECT_EQ(result->uuid, uuid);
}

TEST(LoginTest, ReadLoginStartWithoutAProfileLeavesItUnset) {
    auto packet = make_login_start_packet("Steve");

    auto result = read_login_start(packet.data(), packet.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_uuid);
}

TEST(LoginTest, ReadLoginStartWrongId) {
    std::vector<std::uint8_t> payload;
    write_string(payload, "Steve");

    std::vector<std::uint8_t> out;
    write_packet(out, 0x01, payload);

    auto result = read_login_start(out.data(), out.size());

    EXPECT_FALSE(result.has_value());
}

TEST(LoginTest, ReadLoginStartEmptyData) {
    auto result = read_login_start(nullptr, 0);
    EXPECT_FALSE(result.has_value());
}

TEST(LoginTest, ReadLoginStartTruncated) {
    auto packet = make_login_start_packet("Steve");


    auto result = read_login_start(packet.data(), packet.size() - 3);

    EXPECT_FALSE(result.has_value());
}

TEST(LoginTest, ReadLoginStartEmptyPayload) {
    std::vector<std::uint8_t> out;
    write_packet(out, 0x00, {});

    auto result = read_login_start(out.data(), out.size());

    EXPECT_FALSE(result.has_value());
}

TEST(LoginTest, ReadLoginStartWithUuid) {
    std::vector<std::uint8_t> payload;
    write_string(payload, "Steve");
    for (int i = 0; i < 16; ++i) {
        payload.push_back(static_cast<std::uint8_t>(i));
    }

    std::vector<std::uint8_t> out;
    write_packet(out, 0x00, payload);

    auto result = read_login_start(out.data(), out.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->username, "Steve");
}


TEST(LoginTest, WriteLoginSuccessRoundTrip) {
    std::array<std::uint8_t, 16> uuid{};
    uuid[0] = 0xAA;

    std::array<std::uint8_t, 16> session_id{};
    session_id[15] = 0xBB;

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(write_login_success(out, "Steve", uuid, session_id));

    auto packet = read_packet(out.data(), out.size());
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(packet->id, 0x02);

    EXPECT_EQ(packet->payload[0], 0xAA);

    EXPECT_EQ(packet->payload[packet->payload.size() - 1], 0xBB);
}

TEST(LoginTest, WriteLoginSuccessEmptyUsername) {
    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 16> uuid{};
    std::array<std::uint8_t, 16> session_id{};

    EXPECT_FALSE(write_login_success(out, "", uuid, session_id));
    EXPECT_TRUE(out.empty());
}

TEST(LoginTest, WriteLoginSuccessTooLongUsername) {
    std::array<std::uint8_t, 16> uuid{};
    std::array<std::uint8_t, 16> session_id{};

    std::string long_name(32768, 'a');

    std::vector<std::uint8_t> out;
    EXPECT_FALSE(write_login_success(out, long_name, uuid, session_id));
}

// The client is answered either way, so the caller needs to know which of the two kinds of
// failure it got: a packet still arriving, or a name the server will not take.
TEST(LoginTest, ReadLoginStartFlagsAnUnusableName) {
    auto packet = make_login_start_packet("ThisNameIsFarTooLong");

    bool rejected = false;
    auto result = read_login_start(packet.data(), packet.size(), &rejected);

    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(rejected);
}

TEST(LoginTest, ReadLoginStartLeavesAnIncompletePacketAlone) {
    auto packet = make_login_start_packet("Steve");

    bool rejected = true;
    auto result = read_login_start(packet.data(), packet.size() - 3, &rejected);

    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(rejected);
}

// Login carries its reason as a JSON text component, not as the binary tag form the later
// states use; a quote in the message has to survive as an escape.
TEST(LoginTest, LoginDisconnectCarriesAJsonComponent) {
    packet_ids().login_disconnect = 0x00;

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(write_login_disconnect(out, "Say \"hi\""));

    auto packet = read_packet(out.data(), out.size());
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(packet->id, 0x00);

    auto json = read_string(packet->payload.data(), packet->payload.size());
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ(json->value, "{\"text\":\"Say \\\"hi\\\"\"}");
}

TEST(LoginTest, ComponentDisconnectCarriesTheTagForm) {
    packet_ids().play_disconnect = 0x20;

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(write_component_disconnect(out, packet_ids().play_disconnect, "Bye"));

    auto packet = read_packet(out.data(), out.size());
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(packet->id, 0x20);
    EXPECT_EQ(packet->payload, terila::nbt::text_component_nbt("Bye"));
}

TEST(LoginTest, ComponentDisconnectWithoutAPacketIdWritesNothing) {
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(write_component_disconnect(out, -1, "Bye"));
    EXPECT_TRUE(out.empty());
}

TEST(LoginTest, WriteLoginSuccessNilUuid) {
    std::array<std::uint8_t, 16> uuid{};
    std::array<std::uint8_t, 16> session_id{};

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(write_login_success(out, "Steve", uuid, session_id));

    auto packet = read_packet(out.data(), out.size());
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(packet->id, 0x02);
}