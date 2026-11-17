#pragma once

// Falling blocks: sand, gravel, concrete powder and the anvils. Vanilla turns the block into
// a FallingBlockEntity and lets it fall as an entity; this runs the same rule on block
// resolution instead, so a falling block never leaves the block overlay and no entity stream
// has to learn about it. Each tick the cell below is tested: while it is something the block
// can pass through (vanilla FallingBlock.isFree: air, fire, liquids or a replaceable block)
// the block moves down one cell and every viewer is told; otherwise it has landed and stays.
//
// Scheduling mirrors the block class: FallingBlock.onPlace and updateShape both schedule a
// tick two ticks out, and that tick starts the fall only while the cell below is free.

#include "world/block_world.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace terila::world {

    // The block kinds whose states fall: SandBlock/ColoredFallingBlock (sand, red sand,
    // gravel), ConcretePowderBlock and AnvilBlock. Suspicious sand and gravel are brushable
    // blocks and do not fall, and scaffolding only drops when it lost its support, which is
    // a different rule.
    inline bool is_falling_block(std::string_view name) noexcept {
        if (name == "minecraft:sand" || name == "minecraft:red_sand" ||
            name == "minecraft:gravel" || name == "minecraft:anvil" ||
            name == "minecraft:chipped_anvil" || name == "minecraft:damaged_anvil") {
            return true;
        }
        static constexpr std::string_view kPowder = "_concrete_powder";
        return name.size() > kPowder.size() &&
            name.compare(name.size() - kPowder.size(), kPowder.size(), kPowder) == 0;
    }

    class FallingBlocks {
    public:
        // Where a position's block comes from: the generated column, with the overlay on top.
        using StateAt = std::function<std::int32_t(const BlockPos&)>;
        // Every cell that actually changed, so the network layer can send it and the region
        // file can save the column once the cascade is over.
        using ChangeSink = std::function<void(const BlockPos&, std::int32_t)>;

        // Vanilla FallingBlock.getDelayAfterPlace().
        static constexpr int kDelayAfterPlace = 2;

        void set_world(BlockWorld* world, std::int32_t air_state,
            std::int32_t min_y, std::int32_t max_y) {
            world_ = world;
            air_state_ = air_state;
            min_y_ = min_y;
            max_y_ = max_y;
        }

        // `falling` is the set of states that fall; `passes_through` the states they fall
        // through, which is the game's #minecraft:replaceable tag plus the liquids.
        void set_states(std::unordered_set<std::int32_t> falling,
            std::unordered_set<std::int32_t> passes_through) {
            falling_ = std::move(falling);
            passes_through_ = std::move(passes_through);
        }

        void set_state_at(StateAt state_at) { state_at_ = std::move(state_at); }
        void set_change_sink(ChangeSink sink) { sink_ = std::move(sink); }

        bool falls(std::int32_t state) const {
            return falling_.find(state) != falling_.end();
        }
        bool passes_through(std::int32_t state) const {
            return passes_through_.find(state) != passes_through_.end();
        }
        std::size_t falling_state_count() const noexcept { return falling_.size(); }
        std::size_t passes_through_count() const noexcept { return passes_through_.size(); }

        // A block changed: the cell itself (a sand that was just placed) and the cell above
        // it (a sand that just lost its support) get a scheduled tick, the way updateShape
        // tells a falling block that its neighbourhood moved.
        void block_changed(const BlockPos& pos) {
            schedule(pos);
            schedule(BlockPos{ pos.x, pos.y + 1, pos.z });
        }

        std::size_t pending() const noexcept { return queue_.size(); }

        void tick() {
            if (queue_.empty()) {
                return;
            }

            // The batch is drained so a block that moves schedules itself for the next tick
            // instead of this pass walking a queue that keeps growing underneath it.
            batch_.assign(queue_.begin(), queue_.end());
            queue_.clear();
            queued_.clear();

            for (auto& job : batch_) {
                if (job.delay > 0) {
                    --job.delay;
                    enqueue(job);
                    continue;
                }
                step(job);
            }
        }

    private:
        struct Job {
            BlockPos pos;
            int delay = 0;
        };

        // x and z are 26 bits (the world border), y is 12: positions never collide.
        static std::uint64_t key_of(const BlockPos& pos) noexcept {
            const auto x = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(pos.x) & 0x3FFFFFFu);
            const auto z = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(pos.z) & 0x3FFFFFFu);
            const auto y = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(pos.y + 4096) & 0xFFFu);
            return (x << 38) | (z << 12) | y;
        }

        std::int32_t state_at(const BlockPos& pos) const {
            const std::int32_t generated = state_at_ ? state_at_(pos) : air_state_;
            return world_ == nullptr ? generated : world_->get(pos, generated);
        }

        void schedule(const BlockPos& pos) {
            if (pos.y < min_y_ || pos.y > max_y_) {
                return;
            }
            if (!falls(state_at(pos))) {
                return;
            }
            enqueue(Job{ pos, kDelayAfterPlace });
        }

        void enqueue(const Job& job) {
            if (!queued_.insert(key_of(job.pos)).second) {
                return;
            }
            queue_.push_back(job);
        }

        void step(Job& job) {
            if (world_ == nullptr) {
                return;
            }

            const std::int32_t state = state_at(job.pos);
            if (!falls(state)) {
                return;   // dug out or replaced while it was waiting
            }

            const BlockPos below{ job.pos.x, job.pos.y - 1, job.pos.z };
            if (below.y < min_y_ || !passes_through(state_at(below))) {
                return;   // landed: whatever is under it holds it from here on
            }

            const std::int32_t from_y = job.pos.y;
            world_->set(below, state);
            world_->set(job.pos, air_state_);
            if (sink_) {
                sink_(job.pos, air_state_);
                sink_(below, state);
            }

            job.pos = below;
            enqueue(job);
            // The block that stood on top of this one just lost its support, which is the
            // updateShape a stack of sand needs to follow itself down.
            schedule(BlockPos{ below.x, from_y + 1, below.z });
        }

        BlockWorld* world_ = nullptr;
        std::int32_t air_state_ = 0;
        std::int32_t min_y_ = -64;
        std::int32_t max_y_ = 319;
        std::unordered_set<std::int32_t> falling_;
        std::unordered_set<std::int32_t> passes_through_;
        StateAt state_at_;
        ChangeSink sink_;
        std::deque<Job> queue_;
        std::unordered_set<std::uint64_t> queued_;
        std::vector<Job> batch_;
    };

} // namespace terila::world
