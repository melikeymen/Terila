#include "async/task.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace async = terila::async;

namespace {

    async::Task<int> add_after_yield(int value) {
        co_await async::Yield{};
        co_return value + 1;
    }

    async::Task<int> chain(int start) {
        const int middle = co_await add_after_yield(start);
        co_return middle * 2;
    }

    async::Task<std::string> countdown(int& steps) {
        while (steps > 0) {
            --steps;
            co_await async::Yield{};
        }
        co_return "done";
    }

} // namespace

TEST(AsyncTaskTest, WaitsForAValue) {
    EXPECT_EQ(add_after_yield(1).wait(), 2);
}

TEST(AsyncTaskTest, ChainsWithoutAScheduler) {
    EXPECT_EQ(chain(3).wait(), 8);
}

TEST(AsyncTaskTest, RunsOneStepPerResume) {
    int steps = 3;
    auto task = countdown(steps);
    // The body does not run until it is asked to.
    EXPECT_EQ(steps, 3);

    EXPECT_TRUE(task.step());
    EXPECT_EQ(steps, 2);
    EXPECT_TRUE(task.step());
    EXPECT_EQ(steps, 1);
    EXPECT_TRUE(task.step());
    EXPECT_EQ(steps, 0);
    EXPECT_FALSE(task.step());
    EXPECT_EQ(task.take(), std::string("done"));
}

TEST(AsyncTaskTest, ForwardsAFailureToTheWaiter) {
    const auto failing = []() -> async::Task<int> {
        throw std::runtime_error("lookup failed");
        co_return 0;
    };
    EXPECT_THROW(failing().wait(), std::runtime_error);
}


TEST(AsyncTaskTest, StepsThroughAChildTask) {
    auto task = chain(3);
    // The child yields once, so the first step ends inside add_after_yield. The second step
    // has to resume that child, which then hands control back to the parent.
    EXPECT_TRUE(task.step());
    EXPECT_FALSE(task.step());
    EXPECT_EQ(task.take(), 8);
}

TEST(AsyncTaskTest, SteppingReportsFinishedOnlyAfterTheChildReturned) {
    int steps = 2;
    auto task = countdown(steps);
    int seen = 0;
    while (task.step()) {
        ++seen;
        ASSERT_LE(seen, 10) << "stepping must make progress";
    }
    EXPECT_EQ(seen, 2) << "one step per yield";
    EXPECT_EQ(task.take(), std::string("done"));
}
