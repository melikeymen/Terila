#include "world/nether_portal.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <tuple>

namespace {

    using terila::world::BlockPos;
    using terila::world::PortalFrame;

    constexpr std::int32_t kAir = 0;
    constexpr std::int32_t kObsidian = 1;
    constexpr std::int32_t kStone = 2;

    // Only the cells a test writes are non-air, which is all the frame search reads.
    class TestWorld {
    public:
        void set(int x, int y, int z, std::int32_t state) {
            cells_[{ x, y, z }] = state;
        }

        std::int32_t at(int x, int y, int z) const {
            const auto it = cells_.find({ x, y, z });
            return it == cells_.end() ? kAir : it->second;
        }

        // The obsidian ring of an opening that is `width` x `height` empty cells whose
        // bottom-left cell is (x, y, z).
        void frame(int x, int y, int z, int width, int height, bool along_x) {
            const int sx = along_x ? 1 : 0;
            const int sz = along_x ? 0 : 1;
            for (int row = -1; row <= height; ++row) {
                for (int column = -1; column <= width; ++column) {
                    const bool edge = row < 0 || row >= height || column < 0 || column >= width;
                    if (edge) {
                        set(x + sx * column, y + row, z + sz * column, kObsidian);
                    }
                }
            }
        }

    private:
        std::map<std::tuple<int, int, int>, std::int32_t> cells_;
    };

    std::optional<PortalFrame> find(const TestWorld& world, int x, int y, int z) {
        return terila::world::find_portal_frame(BlockPos{ x, y, z }, -64, 319,
            [&world](const BlockPos& pos) { return world.at(pos.x, pos.y, pos.z); },
            [](std::int32_t state) { return state == kObsidian; },
            [](std::int32_t state) { return state == kAir; });
    }

} // namespace

TEST(NetherPortalTest, FindsAClassicFrameStandingAlongX) {
    TestWorld world;
    world.frame(0, 64, 0, 2, 3, true);

    const auto frame = find(world, 0, 64, 0);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->bottom_left.x, 0);
    EXPECT_EQ(frame->bottom_left.y, 64);
    EXPECT_EQ(frame->bottom_left.z, 0);
    EXPECT_EQ(frame->width, 2);
    EXPECT_EQ(frame->height, 3);
    EXPECT_TRUE(frame->along_x);
    EXPECT_EQ(frame->interior().size(), 6u);
}

TEST(NetherPortalTest, FindsTheSameFrameFromAnyCellInsideIt) {
    TestWorld world;
    world.frame(0, 64, 0, 2, 3, true);

    const auto frame = find(world, 1, 66, 0);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->bottom_left.x, 0);
    EXPECT_EQ(frame->bottom_left.y, 64);
    EXPECT_EQ(frame->width, 2);
    EXPECT_EQ(frame->height, 3);
}

TEST(NetherPortalTest, FindsAFrameWhoseOpeningRunsAlongZ) {
    TestWorld world;
    world.frame(0, 64, 0, 4, 5, false);

    const auto frame = find(world, 0, 64, 3);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->bottom_left.z, 0);
    EXPECT_EQ(frame->width, 4);
    EXPECT_EQ(frame->height, 5);
    EXPECT_FALSE(frame->along_x);
}

TEST(NetherPortalTest, AcceptsTheLargestOpeningTheGameAllows) {
    TestWorld world;
    world.frame(0, 64, 0, terila::world::kPortalMaxWidth, terila::world::kPortalMaxHeight,
        true);

    const auto frame = find(world, 10, 70, 0);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->width, 21);
    EXPECT_EQ(frame->height, 21);
}

TEST(NetherPortalTest, RejectsAnOpeningWiderThanTheGameAllows) {
    TestWorld world;
    world.frame(0, 64, 0, terila::world::kPortalMaxWidth + 1,
        terila::world::kPortalMinHeight, true);

    EXPECT_FALSE(find(world, 0, 64, 0).has_value());
}

TEST(NetherPortalTest, RejectsAnOpeningOnlyOneBlockWide) {
    TestWorld world;
    world.frame(0, 64, 0, 1, 3, true);

    EXPECT_FALSE(find(world, 0, 64, 0).has_value());
}

TEST(NetherPortalTest, RejectsAnOpeningOnlyTwoBlocksTall) {
    TestWorld world;
    world.frame(0, 64, 0, 2, 2, true);

    EXPECT_FALSE(find(world, 0, 64, 0).has_value());
}

TEST(NetherPortalTest, RejectsAFrameMissingItsTopRow) {
    TestWorld world;
    world.frame(0, 64, 0, 2, 3, true);
    world.set(0, 67, 0, kAir);
    world.set(1, 67, 0, kAir);

    EXPECT_FALSE(find(world, 0, 64, 0).has_value());
}

TEST(NetherPortalTest, RejectsAFrameMissingASideColumn) {
    TestWorld world;
    world.frame(0, 64, 0, 2, 3, true);
    world.set(2, 65, 0, kAir);

    EXPECT_FALSE(find(world, 0, 64, 0).has_value());
}

TEST(NetherPortalTest, RejectsAnOpeningWithABlockInsideIt) {
    TestWorld world;
    world.frame(0, 64, 0, 2, 3, true);
    world.set(0, 65, 0, kStone);

    EXPECT_FALSE(find(world, 0, 64, 0).has_value());
}

TEST(NetherPortalTest, RejectsACellThatIsNotInsideAFrame) {
    TestWorld world;
    world.set(0, 63, 0, kStone);
    world.set(0, 62, 0, kStone);

    EXPECT_FALSE(find(world, 0, 64, 0).has_value());
}

TEST(NetherPortalTest, RejectsAFrameWhoseBottomRowIsNotOnObsidian) {
    TestWorld world;
    world.frame(0, 64, 0, 2, 3, true);
    world.set(1, 63, 0, kStone);

    EXPECT_FALSE(find(world, 0, 64, 0).has_value());
}
