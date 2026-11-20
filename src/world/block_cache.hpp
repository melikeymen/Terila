#pragma once

// The light engine reads a 15 block halo around every chunk, and one sample costs a full
// block evaluation (caves, ores, trees). Neighbouring chunks share most of those cells, so a
// small direct-mapped table turns the repeats into an array read: a hit is a few nanoseconds,
// a miss just recomputes. Only pristine generation is cached — player edits live in the
// overlay and never come through here — so an entry can never go stale.

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>

namespace terila::world {

    // Block states are small (a few tens of thousands), and a chunk coordinate fits in 26
    // bits: (x, y, z) packs into one 64 bit key.
    inline std::int64_t block_key(std::int32_t x, int y, std::int32_t z) noexcept {
        return static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) & 0x3FFFFFFull) << 38 |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) & 0x3FFFFFFull) << 12 |
            static_cast<std::uint64_t>(y + 2048));
    }

    class BlockSampleCache {
    public:
        static constexpr std::size_t kSlots = 1u << 18;

        struct Stats {
            std::uint64_t hits = 0;
            std::uint64_t misses = 0;
        };

        BlockSampleCache() : slots_(std::make_unique<Slot[]>(kSlots)) {}

        // `salt` separates worlds: two seeds share the table without ever reading each
        // other's blocks, because the tag they look for differs.
        std::optional<std::int32_t> find(std::int64_t key, std::uint64_t salt) const {
            const Slot& slot = slots_[slot_of(key, salt)];
            const std::uint64_t want = tag_of(key, salt);

            if (slot.tag.load(std::memory_order_acquire) != want) {
                return std::nullopt;
            }
            const std::int32_t state = slot.state.load(std::memory_order_relaxed);
            // Another thread may have replaced this slot between the two loads; the tag is
            // read again so a torn pair is a miss and never a wrong block.
            if (slot.tag.load(std::memory_order_relaxed) != want) {
                return std::nullopt;
            }
            return state;
        }

        void insert(std::int64_t key, std::uint64_t salt, std::int32_t state) {
            Slot& slot = slots_[slot_of(key, salt)];
            slot.state.store(state, std::memory_order_relaxed);
            slot.tag.store(tag_of(key, salt), std::memory_order_release);
        }

        // Off only for the benchmark that has to measure the cache's own worth.
        static void set_enabled(bool on) noexcept {
            enabled_flag().store(on, std::memory_order_relaxed);
        }
        static bool is_enabled() noexcept {
            return enabled_flag().load(std::memory_order_relaxed);
        }

        static void count_hit() noexcept { hits().fetch_add(1, std::memory_order_relaxed); }
        static void count_miss() noexcept { misses().fetch_add(1, std::memory_order_relaxed); }

        static Stats stats() noexcept { return Stats{ hits().load(), misses().load() }; }
        static void reset_stats() noexcept {
            hits().store(0);
            misses().store(0);
        }

    private:
        struct Slot {
            std::atomic<std::uint64_t> tag{ 0 };
            std::atomic<std::int32_t> state{ 0 };
        };

        static std::atomic<bool>& enabled_flag() noexcept {
            static std::atomic<bool> flag{ true };
            return flag;
        }

        static std::atomic<std::uint64_t>& hits() noexcept {
            static std::atomic<std::uint64_t> counter{ 0 };
            return counter;
        }

        static std::atomic<std::uint64_t>& misses() noexcept {
            static std::atomic<std::uint64_t> counter{ 0 };
            return counter;
        }

        static std::uint64_t mix(std::uint64_t value) noexcept {
            value ^= value >> 33;
            value *= 0xff51afd7ed558ccdull;
            value ^= value >> 33;
            value *= 0xc4ceb9fe1a85ec53ull;
            value ^= value >> 33;
            return value;
        }

        static std::uint64_t tag_of(std::int64_t key, std::uint64_t salt) noexcept {
            // Never zero, so a cleared slot cannot be mistaken for a hit.
            return (static_cast<std::uint64_t>(key) ^ salt) + 1;
        }

        static std::size_t slot_of(std::int64_t key, std::uint64_t salt) noexcept {
            return static_cast<std::size_t>(mix(static_cast<std::uint64_t>(key) ^ salt)) &
                (kSlots - 1);
        }

        std::unique_ptr<Slot[]> slots_;
    };

    // A terrain column is a pure function of its cell coordinate, and the light halo of a
    // chunk reaches fifteen blocks into its neighbours: the workers generating neighbouring
    // chunks ask for the same cells at the same time, and every one of those cells costs a
    // walk down the density field. This table is shared by all of them, so a cell is computed
    // once instead of once per chunk that looks at it.
    //
    // Reads and writes race by design: a value that is being replaced is served as a miss
    // (the reader recomputes) rather than with a torn copy, which a stamp around the write
    // detects. No lock, because the writer that wins is the only one that stores.
    template<class Value>
    class CellCache {
    public:
        static constexpr std::size_t kSlots = 1u << 14;

        CellCache() : slots_(std::make_unique<Slot[]>(kSlots)) {}

        std::optional<Value> find(std::int64_t key, std::uint64_t salt) const {
            const Slot& slot = slots_[slot_of(key, salt)];
            const std::uint64_t want = tag_of(key, salt);
            if (slot.tag.load(std::memory_order_acquire) != want) {
                return std::nullopt;
            }
            const std::uint32_t before = slot.stamp.load(std::memory_order_acquire);
            if ((before & 1u) != 0) {
                return std::nullopt;
            }
            const Value value = slot.value;
            if (slot.stamp.load(std::memory_order_acquire) != before) {
                return std::nullopt;
            }
            return value;
        }

        void insert(std::int64_t key, std::uint64_t salt, const Value& value) {
            Slot& slot = slots_[slot_of(key, salt)];
            const std::uint32_t stamp = slot.stamp.load(std::memory_order_relaxed);
            slot.stamp.store(stamp | 1u, std::memory_order_release);
            slot.value = value;
            slot.stamp.store(stamp + 2u, std::memory_order_release);
            slot.tag.store(tag_of(key, salt), std::memory_order_release);
        }

    private:
        struct Slot {
            std::atomic<std::uint64_t> tag{ 0 };
            Value value{};
            std::atomic<std::uint32_t> stamp{ 0 };
        };

        static std::uint64_t mix(std::uint64_t value) noexcept {
            value ^= value >> 33;
            value *= 0xff51afd7ed558ccdull;
            value ^= value >> 33;
            value *= 0xc4ceb9fe1a85ec53ull;
            value ^= value >> 33;
            return value;
        }

        // Never zero, so a cleared slot cannot be mistaken for a hit.
        static std::uint64_t tag_of(std::int64_t key, std::uint64_t salt) noexcept {
            return (static_cast<std::uint64_t>(key) ^ salt) + 1;
        }

        static std::size_t slot_of(std::int64_t key, std::uint64_t salt) noexcept {
            return static_cast<std::size_t>(mix(static_cast<std::uint64_t>(key) ^ salt)) &
                (kSlots - 1);
        }

        std::unique_ptr<Slot[]> slots_;
    };

} // namespace terila::world
