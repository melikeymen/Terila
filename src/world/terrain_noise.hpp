#pragma once

// Noise primitives behind the terrain generator: the position hashes features are placed
// with, and Perlin noise stacked into octaves the way the game's noise router uses it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace terila::world::terrain::detail {

    inline std::uint32_t hash3(std::int32_t x, std::int32_t z, std::uint32_t seed) {
        std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::int64_t>(x)) *
            0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint64_t>(static_cast<std::int64_t>(z)) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<std::uint64_t>(seed) * 0x165667B19E3779F9ull;
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return static_cast<std::uint32_t>(h >> 32);
    }

    inline std::uint64_t hash64(std::uint64_t v) {
        v ^= v >> 33;
        v *= 0xFF51AFD7ED558CCDull;
        v ^= v >> 33;
        v *= 0xC4CEB9FE1A85EC53ull;
        v ^= v >> 33;
        return v;
    }

    inline float fade(float t) {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    inline float lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }

    // Perlin noise, the shape the game's ImprovedNoise has: one gradient per lattice corner,
    // taken from a permutation table seeded once per channel.
    class Perlin {
    public:
        void init(std::uint64_t seed) {
            for (int i = 0; i < 256; ++i) {
                order_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
            }
            std::uint64_t state = hash64(seed | 1ull);
            for (int i = 255; i > 0; --i) {
                state = hash64(state + 0x9E3779B97F4A7C15ull);
                const int j = static_cast<int>(state % static_cast<std::uint64_t>(i + 1));
                std::swap(order_[static_cast<std::size_t>(i)],
                    order_[static_cast<std::size_t>(j)]);
            }
            for (int i = 0; i < 256; ++i) {
                perm_[static_cast<std::size_t>(i)] = order_[static_cast<std::size_t>(i)];
                perm_[static_cast<std::size_t>(i) + 256] = order_[static_cast<std::size_t>(i)];
            }
        }

        float sample(float x, float y, float z) const {
            const auto xi = static_cast<int>(std::floor(x)) & 255;
            const auto yi = static_cast<int>(std::floor(y)) & 255;
            const auto zi = static_cast<int>(std::floor(z)) & 255;
            const float fx = x - std::floor(x);
            const float fy = y - std::floor(y);
            const float fz = z - std::floor(z);
            const float u = fade(fx);
            const float v = fade(fy);
            const float w = fade(fz);

            const int a = perm_[static_cast<std::size_t>(xi)] + yi;
            const int aa = perm_[static_cast<std::size_t>(a)] + zi;
            const int ab = perm_[static_cast<std::size_t>(a + 1)] + zi;
            const int b = perm_[static_cast<std::size_t>(xi + 1)] + yi;
            const int ba = perm_[static_cast<std::size_t>(b)] + zi;
            const int bb = perm_[static_cast<std::size_t>(b + 1)] + zi;

            const auto dot = [](int hash, float dx, float dy, float dz) {
                // The twelve edge gradients of the cube.
                const int h = hash & 15;
                const float u = h < 8 ? dx : dy;
                const float v = h < 4 ? dy : (h == 12 || h == 14 ? dx : dz);
                return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
            };

            const float x1 = lerp(dot(perm_[static_cast<std::size_t>(aa)], fx, fy, fz),
                dot(perm_[static_cast<std::size_t>(ba)], fx - 1.0f, fy, fz), u);
            const float x2 = lerp(dot(perm_[static_cast<std::size_t>(ab)], fx, fy - 1.0f, fz),
                dot(perm_[static_cast<std::size_t>(bb)], fx - 1.0f, fy - 1.0f, fz), u);
            const float y1 = lerp(x1, x2, v);

            const float x3 = lerp(dot(perm_[static_cast<std::size_t>(aa + 1)], fx, fy, fz - 1.0f),
                dot(perm_[static_cast<std::size_t>(ba + 1)], fx - 1.0f, fy, fz - 1.0f), u);
            const float x4 = lerp(
                dot(perm_[static_cast<std::size_t>(ab + 1)], fx, fy - 1.0f, fz - 1.0f),
                dot(perm_[static_cast<std::size_t>(bb + 1)], fx - 1.0f, fy - 1.0f, fz - 1.0f), u);
            const float y2 = lerp(x3, x4, v);

            // Perlin peaks around 1.1 for these gradients; keep the field inside -1..1 so the
            // climate thresholds mean something.
            return std::clamp(lerp(y1, y2, w) * 1.25f, -1.0f, 1.0f);
        }

        float sample2(float x, float z) const {
            return sample(x, 0.0f, z);
        }

    private:
        std::array<std::uint8_t, 256> order_{};
        std::array<std::uint8_t, 512> perm_{};
    };

    // One channel of the noise router: octaves at doubled frequency and halved weight,
    // normalised so the result stays inside -1..1 whatever the octave count is.
    class OctaveNoise {
    public:
        void init(std::uint64_t seed, int octaves, float first_frequency) {
            perlin_.init(seed);
            octaves_ = std::max(1, octaves);
            first_ = first_frequency;
        }

        float sample(float x, float y, float z) const {
            float total = 0.0f;
            float weight = 1.0f;
            float norm = 0.0f;
            float frequency = first_;
            for (int octave = 0; octave < octaves_; ++octave) {
                total += perlin_.sample(x * frequency, y * frequency, z * frequency) * weight;
                norm += weight;
                weight *= 0.5f;
                frequency *= 2.0f;
            }
            return norm > 0.0f ? total / norm : 0.0f;
        }

        float sample2(float x, float z) const {
            return sample(x, 0.0f, z);
        }

    private:
        Perlin perlin_;
        int octaves_ = 1;
        float first_ = 1.0f;
    };

} // namespace terila::world::terrain::detail
