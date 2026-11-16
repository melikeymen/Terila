#include "version/features.hpp"
#include "version/mc_version.hpp"

#include <gtest/gtest.h>

namespace {

    // Flags are macros, so the answer is read once here and asserted as a value.
#ifdef TERILA_HAS_CHUNK_BATCH
    constexpr bool has_chunk_batch = true;
#else
    constexpr bool has_chunk_batch = false;
#endif

#ifdef TERILA_HAS_REGISTRY_DATA_PACKET
    constexpr bool has_registry_data_packet = true;
#else
    constexpr bool has_registry_data_packet = false;
#endif

}

TEST(Version, code_orders_like_a_version) {
    EXPECT_LT(TERILA_MC_CODE(26, 3), TERILA_MC_CODE(26, 10));
    EXPECT_LT(TERILA_MC_CODE(26, 10), TERILA_MC_CODE(27, 0));
    EXPECT_EQ(TERILA_MC_CODE(26, 3), 2603);
}

TEST(Version, floor_is_26_3) {
    EXPECT_EQ(TERILA_MC_FLOOR, TERILA_MC_CODE(26, 3));
    EXPECT_GE(TERILA_MC_TARGET, TERILA_MC_FLOOR);
}

TEST(Version, table_starts_at_the_floor_and_is_sorted) {
    constexpr auto count = terila::version::row_count;
    ASSERT_GT(count, 0u);
    EXPECT_EQ(terila::version::table[0].code, TERILA_MC_FLOOR);

    for (std::size_t i = 1; i < count; ++i) {
        EXPECT_LT(terila::version::table[i - 1].code, terila::version::table[i].code)
            << "row " << i << " is not newer than the one before it";
    }
}

TEST(Version, target_row_describes_the_compiled_protocol) {
    EXPECT_EQ(terila::version::target.code, TERILA_MC_TARGET);
    EXPECT_FALSE(terila::version::name().empty());
    EXPECT_FALSE(terila::version::known_packs().empty());
    EXPECT_GT(terila::version::protocol(), 0);
    EXPECT_GT(terila::version::data_version(), 0);
    EXPECT_NE(terila::version::label().find(terila::version::name()), std::string::npos);
}

TEST(Version, a_dump_from_another_protocol_is_a_mismatch) {
    EXPECT_TRUE(terila::version::matches_source(terila::version::protocol()));
    EXPECT_TRUE(terila::version::matches_source(-1));
    EXPECT_FALSE(terila::version::matches_source(terila::version::protocol() + 1));
}

TEST(Version, the_floor_keeps_its_features) {
    EXPECT_TRUE(has_chunk_batch);
    EXPECT_TRUE(has_registry_data_packet);
}
