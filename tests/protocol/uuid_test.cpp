#include <gtest/gtest.h>
#include "protocol/uuid.hpp"

#include <array>
#include <cstdint>
#include <string>

static std::string to_hex(const std::array<std::uint8_t, 16>& digest) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (auto byte : digest) {
        out.push_back(hex[(byte >> 4) & 0x0F]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

TEST(UuidTest, Md5MatchesRfc1321Vectors) {
    const auto empty = terila::protocol::detail::md5("");
    const auto abc = terila::protocol::detail::md5("abc");
    const auto sentence = terila::protocol::detail::md5("The quick brown fox jumps over the lazy dog");

    ASSERT_TRUE(empty.has_value());
    ASSERT_TRUE(abc.has_value());
    ASSERT_TRUE(sentence.has_value());
    EXPECT_EQ(to_hex(*empty), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(to_hex(*abc), "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(to_hex(*sentence), "9e107d9d372bb6826bd81d3542a419d6");
}

TEST(UuidTest, OfflineUuidMatchesVanilla) {
    const auto notch = terila::protocol::offline_uuid("Notch");
    const auto steve = terila::protocol::offline_uuid("Steve");

    ASSERT_TRUE(notch.has_value());
    ASSERT_TRUE(steve.has_value());
    EXPECT_EQ(terila::protocol::uuid_to_string(*notch), "b50ad385-829d-3141-a216-7e7d7539ba7f");
    EXPECT_EQ(terila::protocol::uuid_to_string(*steve), "5627dd98-e6be-3c21-b8a8-e92344183641");
}

TEST(UuidTest, OfflineUuidHasVersion3AndRfc4122Variant) {
    const auto uuid = terila::protocol::offline_uuid("Terila");
    ASSERT_TRUE(uuid.has_value());
    EXPECT_EQ((*uuid)[6] & 0xF0, 0x30);
    EXPECT_EQ((*uuid)[8] & 0xC0, 0x80);
}

TEST(UuidTest, DifferentNamesProduceDifferentUuids) {
    const auto alex = terila::protocol::offline_uuid("Alex");
    const auto steve = terila::protocol::offline_uuid("Steve");

    ASSERT_TRUE(alex.has_value());
    ASSERT_TRUE(steve.has_value());
    EXPECT_NE(*alex, *steve);
}
