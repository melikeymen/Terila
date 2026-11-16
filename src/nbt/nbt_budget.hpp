#pragma once

#include "log/logger.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace terila::nbt {

    // A tag payload says how big it is before it contains anything, so a few bytes can ask
    // for gigabytes, and a nesting chain costs one stack frame per level. Every read
    // therefore runs against a budget: how much may be claimed in total, and how deep the
    // tree may go. Going over is malformed input, not a crash.
    class BudgetExceeded : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class ReadBudget {
    public:
        // What a client can send: item components, chat text, entity metadata.
        static ReadBudget from_client() { return ReadBudget(2 * 1024 * 1024, 64); }
        // What we read back from our own files, which are trusted but still large.
        static ReadBudget from_disk() { return ReadBudget(100 * 1024 * 1024, 512); }
        static ReadBudget no_limit() {
            return ReadBudget((std::numeric_limits<std::int64_t>::max)(), 1024);
        }

        explicit ReadBudget(std::int64_t bytes, std::size_t max_depth)
            : allowance_(bytes), depth_limit_(max_depth) {}

        void spend(std::int64_t bytes) const {
            if (bytes < 0) {
                throw BudgetExceeded("tag read asked for a negative allocation");
            }
            if (spent_ + bytes > allowance_) {
                LOG_WARN(Data, "tag payload over budget: {} + {} > {} bytes", spent_, bytes,
                    allowance_);
                throw BudgetExceeded("tag payload is too big: " + std::to_string(spent_ + bytes) +
                    " bytes where " + std::to_string(allowance_) + " are allowed");
            }
            spent_ += bytes;
        }

        // Nesting is a scope: a reader enters one level per container it descends into.
        bool enter() const {
            if (depth_ >= depth_limit_) {
                LOG_WARN(Data, "tag payload nested deeper than {} levels", depth_limit_);
                return false;
            }
            ++depth_;
            return true;
        }

        void leave() const {
            if (depth_ > 0) {
                --depth_;
            }
        }

        std::int64_t spent() const noexcept { return spent_; }
        std::int64_t allowance() const noexcept { return allowance_; }
        std::size_t depth() const noexcept { return depth_; }
        std::size_t depth_limit() const noexcept { return depth_limit_; }

        // Leaves the nesting scope however the read ends, including through a throw.
        struct Level {
            explicit Level(const ReadBudget& budget) : budget_(budget), ok_(budget.enter()) {}
            ~Level() { if (ok_) budget_.leave(); }
            Level(const Level&) = delete;
            Level& operator=(const Level&) = delete;
            bool ok() const noexcept { return ok_; }

        private:
            const ReadBudget& budget_;
            bool ok_;
        };

    private:
        std::int64_t allowance_;
        std::size_t depth_limit_;
        mutable std::int64_t spent_ = 0;
        mutable std::size_t depth_ = 0;
    };

} // namespace terila::nbt
