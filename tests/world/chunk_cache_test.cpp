#include <gtest/gtest.h>

#include "world/chunk_cache.hpp"

#include <cstdint>
#include <vector>

namespace world = terila::world;

namespace {

    world::Chunk chunk_at(std::int32_t cx, std::int32_t cz) {
        world::Chunk chunk;
        chunk.x = cx;
        chunk.z = cz;
        return chunk;
    }

    std::vector<std::uint8_t> payload(std::uint8_t value) {
        return std::vector<std::uint8_t>{ value };
    }

} // namespace

TEST(ChunkCacheTest, ReturnsWhatWasPut) {
    world::ChunkCache cache;
    cache.put(chunk_at(3, -4), payload(7));

    const auto* entry = cache.find(3, -4);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->encoded, payload(7));
    EXPECT_EQ(entry->chunk.x, 3);
    EXPECT_EQ(entry->chunk.z, -4);
    EXPECT_EQ(cache.find(4, -4), nullptr);
}

TEST(ChunkCacheTest, EvictsTheOldestColumnFirst) {
    world::ChunkCache cache(2);
    cache.put(chunk_at(0, 0), payload(0));
    cache.put(chunk_at(1, 0), payload(1));
    cache.put(chunk_at(2, 0), payload(2));

    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(cache.find(0, 0), nullptr);   // the one that was oldest
    EXPECT_NE(cache.find(1, 0), nullptr);
    EXPECT_NE(cache.find(2, 0), nullptr);
}

TEST(ChunkCacheTest, AHitCountsAsFresh) {
    world::ChunkCache cache(2);
    cache.put(chunk_at(0, 0), payload(0));
    cache.put(chunk_at(1, 0), payload(1));
    ASSERT_NE(cache.find(0, 0), nullptr);   // promotes 0

    cache.put(chunk_at(2, 0), payload(2));
    EXPECT_NE(cache.find(0, 0), nullptr);
    EXPECT_EQ(cache.find(1, 0), nullptr);
}

TEST(ChunkCacheTest, ReplacingAColumnKeepsOneEntry) {
    world::ChunkCache cache(4);
    cache.put(chunk_at(0, 0), payload(0));
    cache.put(chunk_at(0, 0), payload(9));

    EXPECT_EQ(cache.size(), 1u);
    const auto* entry = cache.find(0, 0);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->encoded, payload(9));
}

TEST(ChunkCacheTest, ErasingAStaleColumnFreesTheSlot) {
    world::ChunkCache cache(1);
    cache.put(chunk_at(5, 5), payload(5));
    cache.erase(5, 5);

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.find(5, 5), nullptr);
    cache.put(chunk_at(6, 6), payload(6));   // no eviction needed now
    EXPECT_NE(cache.find(6, 6), nullptr);
}
