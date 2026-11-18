#pragma once

// The noise primitives behind the density functions, matching the game's own arithmetic so a
// seed produces the same field. The game derives every noise from the world seed through
// Xoroshiro128++, an MD5 hash of the noise name, and per-octave positional seeds; the value of
// a field is the sum of two Perlin octave stacks, one sampled slightly faster than the other.
//
// Everything here is written from the documented behaviour of that chain (the PRNG, the
// gradient table, the octave weighting, the normalisation target) rather than copied from a
// particular implementation: the structure follows the format, the code is ours.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    inline std::uint64_t mix64(std::uint64_t v) {
        v += 0x9E3779B97F4A7C15ull;
        v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ull;
        v = (v ^ (v >> 27)) * 0x94D049BB133111EBull;
        return v ^ (v >> 31);
    }

    namespace detail {

        inline std::uint32_t rotl32(std::uint32_t value, int bits) {
            return (value << bits) | (value >> (32 - bits));
        }

        // The 128-bit digest is used only as a stable seed, so the hash is implemented here
        // instead of pulling in a crypto dependency.
        inline std::array<std::uint8_t, 16> md5(std::string_view text) {
            static constexpr std::uint32_t kTable[64] = {
                0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
                0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
                0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
                0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
                0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
                0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
                0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
                0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
                0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
                0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
                0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
                0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
                0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
                0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
                0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
                0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
            };
            static constexpr int kShift[64] = {
                7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
            };

            std::vector<std::uint8_t> message(text.begin(), text.end());
            const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8ull;
            message.push_back(0x80);
            while (message.size() % 64 != 56) {
                message.push_back(0);
            }
            for (int i = 0; i < 8; ++i) {
                message.push_back(static_cast<std::uint8_t>(bit_length >> (8 * i)));
            }

            std::uint32_t a0 = 0x67452301u;
            std::uint32_t b0 = 0xefcdab89u;
            std::uint32_t c0 = 0x98badcfeu;
            std::uint32_t d0 = 0x10325476u;

            for (std::size_t offset = 0; offset < message.size(); offset += 64) {
                std::uint32_t words[16]{};
                for (int i = 0; i < 16; ++i) {
                    words[i] = static_cast<std::uint32_t>(message[offset + i * 4]) |
                        (static_cast<std::uint32_t>(message[offset + i * 4 + 1]) << 8) |
                        (static_cast<std::uint32_t>(message[offset + i * 4 + 2]) << 16) |
                        (static_cast<std::uint32_t>(message[offset + i * 4 + 3]) << 24);
                }
                std::uint32_t a = a0;
                std::uint32_t b = b0;
                std::uint32_t c = c0;
                std::uint32_t d = d0;
                for (int i = 0; i < 64; ++i) {
                    std::uint32_t f = 0;
                    int g = 0;
                    if (i < 16) {
                        f = (b & c) | (~b & d);
                        g = i;
                    }
                    else if (i < 32) {
                        f = (d & b) | (~d & c);
                        g = (5 * i + 1) % 16;
                    }
                    else if (i < 48) {
                        f = b ^ c ^ d;
                        g = (3 * i + 5) % 16;
                    }
                    else {
                        f = c ^ (b | ~d);
                        g = (7 * i) % 16;
                    }
                    const std::uint32_t rotated = rotl32(a + f + kTable[i] + words[g], kShift[i]);
                    a = d;
                    d = c;
                    c = b;
                    b = b + rotated;
                }
                a0 += a;
                b0 += b;
                c0 += c;
                d0 += d;
            }

            std::array<std::uint8_t, 16> digest{};
            const std::uint32_t state[4] = { a0, b0, c0, d0 };
            for (int word = 0; word < 4; ++word) {
                for (int byte = 0; byte < 4; ++byte) {
                    digest[static_cast<std::size_t>(word * 4 + byte)] =
                        static_cast<std::uint8_t>(state[word] >> (8 * byte));
                }
            }
            return digest;
        }

    } // namespace detail

    class Xoroshiro {
    public:
        Xoroshiro(std::uint64_t seed_lo, std::uint64_t seed_hi)
            : seed_lo_(seed_lo), seed_hi_(seed_hi) {
            if ((seed_lo_ | seed_hi_) == 0ull) {
                seed_lo_ = 0x9E3779B97F4A7C15ull;
                seed_hi_ = 0x6A09E667F3BCC909ull;
            }
        }

        std::uint64_t next() {
            const std::uint64_t s0 = seed_lo_;
            const std::uint64_t s1 = seed_hi_;
            const std::uint64_t result = rotl(s0 + s1, 17) + s0;
            seed_hi_ = s1 ^ s0;
            seed_lo_ = rotl(s0, 49) ^ seed_hi_ ^ (seed_hi_ << 21);
            seed_hi_ = rotl(seed_hi_, 28);
            return result;
        }

    private:
        static std::uint64_t rotl(std::uint64_t value, int bits) {
            return (value << bits) | (value >> (64 - bits));
        }

        std::uint64_t seed_lo_;
        std::uint64_t seed_hi_;
    };

    class PositionalFactory;

    class RandomSource {
    public:
        RandomSource(std::uint64_t seed_lo, std::uint64_t seed_hi)
            : generator_(seed_lo, seed_hi) {
        }

        // A single long seed is widened into the 128 bits the generator runs on: the low half
        // is offset by the silver ratio, the high half by the golden ratio, and both are
        // stirred with the same mixer the game uses for its seed material.
        static RandomSource from_seed(std::uint64_t seed) {
            const std::uint64_t low = seed ^ 0x6A09E667F3BCC909ull;
            const std::uint64_t high = low + 0x9E3779B97F4A7C15ull;
            return RandomSource(mix_stafford(low), mix_stafford(high));
        }

        std::uint64_t next_long() { return generator_.next(); }

        int next_int() { return static_cast<int>(next_long()); }

        int next_int(int bound) {
            std::uint64_t bits = static_cast<std::uint32_t>(next_int());
            std::uint64_t multiplied = bits * static_cast<std::uint64_t>(bound);
            std::uint64_t fractional = multiplied & 0xFFFFFFFFull;
            if (fractional < static_cast<std::uint64_t>(bound)) {
                const std::uint64_t threshold = static_cast<std::uint64_t>(
                    (0x100000000ull - static_cast<std::uint64_t>(bound)) %
                    static_cast<std::uint64_t>(bound));
                while (fractional < threshold) {
                    bits = static_cast<std::uint32_t>(next_int());
                    multiplied = bits * static_cast<std::uint64_t>(bound);
                    fractional = multiplied & 0xFFFFFFFFull;
                }
            }
            return static_cast<int>(multiplied >> 32);
        }

        double next_double() {
            return static_cast<double>(next_long() >> 11) * static_cast<double>(1.110223e-16f);
        }

        RandomSource fork() {
            const std::uint64_t low = next_long();
            return RandomSource(low, next_long());
        }

        PositionalFactory fork_positional();

    private:
        static std::uint64_t mix_stafford(std::uint64_t z) {
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        Xoroshiro generator_;
    };

    class PositionalFactory {
    public:
        PositionalFactory() = default;

        PositionalFactory(std::uint64_t seed_lo, std::uint64_t seed_hi)
            : seed_lo_(seed_lo), seed_hi_(seed_hi) {
        }

        // Hashing the name gives every named field its own stream without a shared sequence,
        // so the order the registry is walked in cannot change a single value.
        RandomSource from_hash_of(std::string_view name) const {
            const auto digest = detail::md5(name);
            std::uint64_t lo = 0;
            std::uint64_t hi = 0;
            for (int i = 0; i < 8; ++i) {
                lo = (lo << 8) | digest[static_cast<std::size_t>(i)];
                hi = (hi << 8) | digest[static_cast<std::size_t>(8 + i)];
            }
            return RandomSource(lo ^ seed_lo_, hi ^ seed_hi_);
        }

        RandomSource from_seed(std::uint64_t seed) const {
            return RandomSource(seed ^ seed_lo_, seed ^ seed_hi_);
        }

        RandomSource at(int x, int y, int z) const {
            std::uint64_t positional = (static_cast<std::uint64_t>(x * 3129871) &
                0xFFFFFFFFull) ^ (static_cast<std::uint64_t>(z) * 116129781ull) ^
                static_cast<std::uint64_t>(y);
            positional = positional * positional * 42317861ull + positional * 11ull;
            positional >>= 16;
            return RandomSource(positional ^ seed_lo_, seed_hi_);
        }

    private:
        std::uint64_t seed_lo_ = 0;
        std::uint64_t seed_hi_ = 0;
    };

    inline PositionalFactory RandomSource::fork_positional() {
        const std::uint64_t low = next_long();
        const std::uint64_t high = next_long();
        return PositionalFactory(low, high);
    }

    // The gradient lattice: sixteen edge directions, hashed through a permuted table. The
    // sample is a trilinear blend of the eight corner gradients with the smoothstep curve the
    // game uses, so neighbouring lattice cells join without a visible seam.
    class Perlin {
    public:
        Perlin() {
            perms_.fill(0);
        }

        void init(RandomSource& random) {
            offset_x_ = random.next_double() * 256.0;
            offset_y_ = random.next_double() * 256.0;
            offset_z_ = random.next_double() * 256.0;

            for (int i = 0; i < 256; ++i) {
                perms_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
            }
            for (int i = 0; i < 256; ++i) {
                const int other = i + random.next_int(256 - i);
                std::swap(perms_[static_cast<std::size_t>(i)],
                    perms_[static_cast<std::size_t>(other)]);
            }
        }

        // The legacy blended noise asks for an octave from a bare seed rather than a stream.
        void init(std::uint64_t seed) {
            RandomSource random = RandomSource::from_seed(seed);
            init(random);
        }

        double sample(double x, double y, double z) const { return get(x, y, z); }

        // The terrain noise smears the vertical gradient of an octave so a layer stays constant
        // inside a thin slab. The fudge is subtracted from the relative height the gradients
        // see, while the blend still uses the unmodified height.
        float get_smeared(double x_in, double y_in, double z_in, double fudge_scale) const {
            const double x = wrap(x_in) + offset_x_;
            const double y = wrap(y_in) + offset_y_;
            const double z = wrap(z_in) + offset_z_;
            const int floor_x = floor_of(x);
            const int floor_y = floor_of(y);
            const int floor_z = floor_of(z);
            const float relative_x = static_cast<float>(x - static_cast<double>(floor_x));
            const double relative_y = y - static_cast<double>(floor_y);
            const float relative_z = static_cast<float>(z - static_cast<double>(floor_z));

            const double limit = (y_in >= 0.0 && y_in < relative_y) ? y_in : relative_y;
            const double fudge = std::floor(limit / fudge_scale + 1.0e-7) * fudge_scale;
            const float fudged_y = static_cast<float>(relative_y - fudge);

            return lattice(floor_x, floor_y, floor_z, relative_x, fudged_y, relative_z,
                static_cast<float>(relative_y));
        }

        float get(double x_in, double y_in, double z_in) const {
            const double x = wrap(x_in) + offset_x_;
            const double y = wrap(y_in) + offset_y_;
            const double z = wrap(z_in) + offset_z_;
            const int floor_x = floor_of(x);
            const int floor_y = floor_of(y);
            const int floor_z = floor_of(z);
            const float relative_x = static_cast<float>(x - static_cast<double>(floor_x));
            const float relative_y = static_cast<float>(y - static_cast<double>(floor_y));
            const float relative_z = static_cast<float>(z - static_cast<double>(floor_z));
            return lattice(floor_x, floor_y, floor_z, relative_x, relative_y, relative_z,
                relative_y);
        }

    private:
        float lattice(int floor_x, int floor_y, int floor_z, float relative_x,
            float relative_y, float relative_z, float alpha_y) const {
            const int x0 = permute(floor_x);
            const int x1 = permute(floor_x + 1);
            const int xy00 = permute(x0 + floor_y);
            const int xy01 = permute(x0 + floor_y + 1);
            const int xy10 = permute(x1 + floor_y);
            const int xy11 = permute(x1 + floor_y + 1);

            const float d000 = grad_dot(permute(xy00 + floor_z), relative_x, relative_y,
                relative_z);
            const float d100 = grad_dot(permute(xy10 + floor_z), relative_x - 1.0f, relative_y,
                relative_z);
            const float d010 = grad_dot(permute(xy01 + floor_z), relative_x,
                relative_y - 1.0f, relative_z);
            const float d110 = grad_dot(permute(xy11 + floor_z), relative_x - 1.0f,
                relative_y - 1.0f, relative_z);
            const float d001 = grad_dot(permute(xy00 + floor_z + 1), relative_x, relative_y,
                relative_z - 1.0f);
            const float d101 = grad_dot(permute(xy10 + floor_z + 1), relative_x - 1.0f,
                relative_y, relative_z - 1.0f);
            const float d011 = grad_dot(permute(xy01 + floor_z + 1), relative_x,
                relative_y - 1.0f, relative_z - 1.0f);
            const float d111 = grad_dot(permute(xy11 + floor_z + 1), relative_x - 1.0f,
                relative_y - 1.0f, relative_z - 1.0f);

            return lerp3(smoothstep(relative_x), smoothstep(alpha_y),
                smoothstep(relative_z), d000, d100, d010, d110, d001, d101, d011, d111);
        }

        struct Gradient {
            int x;
            int y;
            int z;
        };

        static const Gradient& gradient(int hash) {
            static const Gradient kGradients[16] = {
                { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 },
                { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
                { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 },
                { 1, 1, 0 }, { 0, -1, 1 }, { -1, 1, 0 }, { 0, -1, -1 },
            };
            return kGradients[hash & 15];
        }

        static float grad_dot(int hash, float x, float y, float z) {
            const Gradient& g = gradient(hash);
            return static_cast<float>(g.x) * x + static_cast<float>(g.y) * y +
                static_cast<float>(g.z) * z;
        }

        static float smoothstep(float value) {
            return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
        }

        static float lerp(float alpha, float first, float second) {
            return first + alpha * (second - first);
        }

        static float lerp3(float alpha_x, float alpha_y, float alpha_z, float x000, float x100,
            float x010, float x110, float x001, float x101, float x011, float x111) {
            return lerp(alpha_z,
                lerp(alpha_y, lerp(alpha_x, x000, x100), lerp(alpha_x, x010, x110)),
                lerp(alpha_y, lerp(alpha_x, x001, x101), lerp(alpha_x, x011, x111)));
        }

        // Far from the origin the lattice index would lose precision; the game folds the
        // coordinate back into a band it can still represent exactly.
        static double wrap(double value) {
            constexpr double kHalf = 1.6777216e7;
            constexpr double kPeriod = 3.3554432e7;
            if (value >= -kHalf && value < kHalf) {
                return value;
            }
            return value - std::floor(value / kPeriod + 0.5) * kPeriod;
        }

        static int floor_of(double value) {
            return static_cast<int>(std::floor(value));
        }

        int permute(int value) const {
            return perms_[static_cast<std::size_t>(value & 0xFF)];
        }

        std::array<std::uint8_t, 256> perms_{};
        double offset_x_ = 0.0;
        double offset_y_ = 0.0;
        double offset_z_ = 0.0;
    };

    // One entry of the noise registry, as the extractor dumped it.
    struct NoiseSpec {
        int base_octave = 0;
        double base_amplitude = 1.0;
        int octave_count = 1;
        std::vector<double> amplitude_modifiers;
        bool normalize = true;
    };

    class NormalNoise {
    public:
        void init(const NoiseSpec& spec, RandomSource& random) {
            layers_.clear();

            const int count = std::max(1, spec.octave_count);
            double amplitude = spec.base_amplitude;
            if (spec.normalize) {
                amplitude = spec.base_amplitude *
                    (std::pow(0.5, static_cast<double>(-(count - 1))) /
                        (std::pow(0.5, static_cast<double>(-count)) - 1.0));
            }
            double frequency = std::pow(2.0, static_cast<double>(spec.base_octave));

            struct Octave {
                int index = 0;
                double frequency = 1.0;
                double amplitude = 1.0;
            };
            std::vector<Octave> octaves;
            double target_amplitude = 0.0;
            double deviation_squared = 0.0;
            for (int i = 0; i < count; ++i) {
                const double modifier = spec.amplitude_modifiers.empty()
                    ? 1.0
                    : (i < static_cast<int>(spec.amplitude_modifiers.size())
                        ? spec.amplitude_modifiers[static_cast<std::size_t>(i)] : 1.0);
                if (modifier != 0.0) {
                    Octave octave;
                    octave.index = spec.base_octave + i;
                    octave.frequency = frequency;
                    octave.amplitude = amplitude * modifier;
                    octaves.push_back(octave);
                    target_amplitude += std::abs(octave.amplitude);
                    const double layer = 0.2702247831245211 * std::abs(octave.amplitude);
                    deviation_squared += layer * layer;
                }
                frequency *= 2.0;
                amplitude *= 0.5;
            }

            // The octave stack is scaled so its spread matches the game's target deviation,
            // otherwise density thresholds would mean different heights per field.
            const double deviation = std::sqrt(deviation_squared);
            double normalization = 0.0;
            if (deviation > 0.0) {
                normalization = (target_amplitude / 3.0) / (deviation * std::sqrt(2.0));
            }

            PositionalFactory first_factory = random.fork_positional();
            PositionalFactory second_factory = random.fork_positional();
            for (const auto& octave : octaves) {
                const std::string seed = "octave_" + std::to_string(octave.index);
                RandomSource first_random = first_factory.from_hash_of(seed);
                RandomSource second_random = second_factory.from_hash_of(seed);
                Layer first;
                first.noise.init(first_random);
                first.frequency = octave.frequency;
                first.amplitude = static_cast<float>(normalization * octave.amplitude);
                Layer second;
                second.noise.init(second_random);
                second.frequency = octave.frequency * 1.0181268882175227;
                second.amplitude = static_cast<float>(normalization * octave.amplitude);
                layers_.push_back(first);
                layers_.push_back(second);
            }
        }

        float sample(double x, double y, double z) const {
            float value = 0.0f;
            for (const auto& layer : layers_) {
                value += layer.amplitude * layer.noise.get(x * layer.frequency,
                    y * layer.frequency, z * layer.frequency);
            }
            return value;
        }

        bool empty() const noexcept { return layers_.empty(); }

    private:
        struct Layer {
            Perlin noise;
            double frequency = 1.0;
            float amplitude = 1.0f;
        };

        std::vector<Layer> layers_;
    };

    // The game's noise registry, as dumped to registries/.../worldgen/noise.json. Each field
    // is seeded from the hash of its own name, so the table can be walked in any order.
    class NoiseRegistry {
    public:
        bool load(std::uint32_t world_seed, const nlohmann::json& root) {
            noises_.clear();
            const auto* entries = find_entries(root);
            if (entries == nullptr) {
                return false;
            }
            PositionalFactory factory = RandomSource::from_seed(world_seed).fork_positional();
            for (const auto& entry : *entries) {
                if (!entry.contains("id") || !entry.contains("data")) {
                    continue;
                }
                const auto& data = entry["data"];
                NoiseSpec spec;
                spec.base_octave = data.value("base_octave", 0);
                spec.base_amplitude = data.value("base_amplitude", 1.0);
                spec.octave_count = data.value("octave_count", 1);
                if (data.contains("normalize")) {
                    if (data["normalize"].is_boolean()) {
                        spec.normalize = data["normalize"].get<bool>();
                    }
                    else if (data["normalize"].is_string()) {
                        spec.normalize = data["normalize"].get<std::string>() != "legacy";
                    }
                }
                if (data.contains("amplitude_modifiers") &&
                    data["amplitude_modifiers"].is_array()) {
                    for (const auto& value : data["amplitude_modifiers"]) {
                        if (value.is_number()) {
                            spec.amplitude_modifiers.push_back(value.get<double>());
                        }
                    }
                }

                const std::string id = entry["id"].get<std::string>();
                RandomSource random = factory.from_hash_of(id);
                noises_[id].init(spec, random);
            }
            return !noises_.empty();
        }

        const NormalNoise* find(std::string_view id) const {
            const auto it = noises_.find(std::string(id));
            return it == noises_.end() ? nullptr : &it->second;
        }

        std::size_t size() const noexcept { return noises_.size(); }

    private:
        static const nlohmann::json* find_entries(const nlohmann::json& root) {
            if (root.contains("registries") && root["registries"].contains("entries")) {
                return &root["registries"]["entries"];
            }
            if (root.contains("data") && root["data"].is_array()) {
                return &root["data"];
            }
            return nullptr;
        }

        std::unordered_map<std::string, NormalNoise> noises_;
    };

} // namespace terila::world::worldgen
