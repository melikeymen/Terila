#pragma once

// Java's own random, the 48 bit linear congruential generator the game reaches for wherever a
// placement has to be derived from a seed and a chunk position. The noise fields run on a
// different generator on purpose; this one has to agree bit for bit, because the carver start,
// the feature order and the structure placement all line up through it.

#include <cstdint>

namespace terila::world::worldgen {

    class LegacyRandom {
    public:
        static constexpr std::uint64_t kMultiplier = 0x5DEECE66Dull;
        static constexpr std::uint64_t kAddend = 0xBull;
        static constexpr std::uint64_t kMask = (1ull << 48) - 1;

        LegacyRandom() { set_seed(0); }

        explicit LegacyRandom(std::uint64_t seed) { set_seed(seed); }

        void set_seed(std::uint64_t seed) { state_ = (seed ^ kMultiplier) & kMask; }

        // The placement seed: the two axes are folded together through two draws, so two
        // neighbouring chunks never walk the same path even when only one axis differs.
        void set_large_feature_seed(std::uint64_t seed, int x, int z) {
            set_seed(seed);
            const std::int64_t x_seed = next_long() | 1;
            const std::int64_t z_seed = next_long() | 1;
            set_seed(static_cast<std::uint64_t>(
                static_cast<std::int64_t>(x) * x_seed ^ static_cast<std::int64_t>(z) * z_seed));
        }

        int next_bits(int bits) {
            state_ = (state_ * kMultiplier + kAddend) & kMask;
            return static_cast<int>(state_ >> (48 - bits));
        }

        int next_int() { return next_bits(32); }

        int next_int(int bound) {
            if (bound <= 0) {
                return 0;
            }
            // A power of two is a shift; anything else draws again until the value is inside
            // the range, which is what keeps the distribution flat.
            if ((bound & -bound) == bound) {
                return static_cast<int>((static_cast<std::int64_t>(bound) * next_bits(31)) >> 31);
            }
            int bits = 0;
            int value = 0;
            do {
                bits = next_bits(31);
                value = bits % bound;
            } while (bits - value + (bound - 1) < 0);
            return value;
        }

        std::int64_t next_long() {
            return (static_cast<std::int64_t>(next_bits(32)) << 32) + next_bits(32);
        }

        float next_float() {
            return static_cast<float>(next_bits(24)) / static_cast<float>(1 << 24);
        }

        double next_double() {
            const std::int64_t high = next_bits(26);
            const std::int64_t low = next_bits(27);
            return static_cast<double>((high << 27) + low) * 1.1102230246251565e-16;
        }

    private:
        std::uint64_t state_ = 0;
    };

} // namespace terila::world::worldgen
