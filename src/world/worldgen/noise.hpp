#pragma once

// Noise primitives for the data-driven generator.
//
// The game names its noise fields ("minecraft:continentalness", "minecraft:cave_cheese")
// and stores their shape as data: a base octave, a base amplitude, an octave count and
// optional per-octave amplitude modifiers. This reproduces that layering — octaves at
// doubled frequency and halved amplitude, normalised so a field has the same spread the game
// gives it (one third of the summed amplitude) — with our own Perlin implementation.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    inline std::uint64_t mix64(std::uint64_t v) {
        v += 0x9E3779B97F4A7C15ull;
        v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ull;
        v = (v ^ (v >> 27)) * 0x94D049BB133111EBull;
        return v ^ (v >> 31);
    }

    // Classic lattice Perlin noise: one gradient per corner, fade-curved interpolation. The
    // result is roughly -1..1, which is the range the octave stack expects.
    class Perlin {
    public:
        void init(std::uint64_t seed) {
            for (int i = 0; i < 256; ++i) {
                order_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
            }
            std::uint64_t state = mix64(seed | 1ull);
            for (int i = 255; i > 0; --i) {
                state = mix64(state);
                const auto j = static_cast<int>(state % static_cast<std::uint64_t>(i + 1));
                std::swap(order_[static_cast<std::size_t>(i)],
                    order_[static_cast<std::size_t>(j)]);
            }
            for (int i = 0; i < 256; ++i) {
                perm_[static_cast<std::size_t>(i)] = order_[static_cast<std::size_t>(i)];
                perm_[static_cast<std::size_t>(i) + 256] = order_[static_cast<std::size_t>(i)];
            }
        }

        double sample(double x, double y, double z) const {
            const auto xi = static_cast<int>(std::floor(x)) & 255;
            const auto yi = static_cast<int>(std::floor(y)) & 255;
            const auto zi = static_cast<int>(std::floor(z)) & 255;
            const double fx = x - std::floor(x);
            const double fy = y - std::floor(y);
            const double fz = z - std::floor(z);
            const double u = fade(fx);
            const double v = fade(fy);
            const double w = fade(fz);

            const int a = perm_[static_cast<std::size_t>(xi)] + yi;
            const int aa = perm_[static_cast<std::size_t>(a & 255)] + zi;
            const int ab = perm_[static_cast<std::size_t>((a + 1) & 255)] + zi;
            const int b = perm_[static_cast<std::size_t>(xi + 1)] + yi;
            const int ba = perm_[static_cast<std::size_t>(b & 255)] + zi;
            const int bb = perm_[static_cast<std::size_t>((b + 1) & 255)] + zi;

            const double x1 = lerp(dot(perm_[static_cast<std::size_t>(aa & 255)], fx, fy, fz),
                dot(perm_[static_cast<std::size_t>(ba & 255)], fx - 1.0, fy, fz), u);
            const double x2 = lerp(dot(perm_[static_cast<std::size_t>(ab & 255)], fx, fy - 1.0, fz),
                dot(perm_[static_cast<std::size_t>(bb & 255)], fx - 1.0, fy - 1.0, fz), u);
            const double y1 = lerp(x1, x2, v);

            const double x3 = lerp(
                dot(perm_[static_cast<std::size_t>((aa + 1) & 255)], fx, fy, fz - 1.0),
                dot(perm_[static_cast<std::size_t>((ba + 1) & 255)], fx - 1.0, fy, fz - 1.0), u);
            const double x4 = lerp(
                dot(perm_[static_cast<std::size_t>((ab + 1) & 255)], fx, fy - 1.0, fz - 1.0),
                dot(perm_[static_cast<std::size_t>((bb + 1) & 255)], fx - 1.0, fy - 1.0, fz - 1.0),
                u);
            const double y2 = lerp(x3, x4, v);

            return lerp(y1, y2, w);
        }

    private:
        static double fade(double t) {
            return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        }

        static double lerp(double a, double b, double t) {
            return a + (b - a) * t;
        }

        static double dot(int hash, double x, double y, double z) {
            // The twelve edge gradients: enough directions for a cube lattice, cheap to pick
            // and symmetrical enough that the noise has no visible axis bias.
            const int h = hash & 15;
            const double u = h < 8 ? x : y;
            const double v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
            return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
        }

        std::array<std::uint8_t, 256> order_{};
        std::array<std::uint8_t, 512> perm_{};
    };

    // One entry of the game's noise registry, as the extractor dumped it.
    struct NoiseSpec {
        int base_octave = 0;
        double base_amplitude = 1.0;
        int octave_count = 1;
        std::vector<double> amplitude_modifiers;
    };

    class NormalNoise {
    public:
        void init(const NoiseSpec& spec, std::uint64_t seed) {
            octaves_.clear();
            normalization_ = 1.0;

            const int count = std::max(1, spec.octave_count);
            // The octave builder of the game rescales the base amplitude so the stack keeps
            // its mean shape whatever the octave count is; the deviation normalisation below
            // then pins the spread to a third of the summed amplitude.
            const double amplitude_scale = std::pow(0.5, static_cast<double>(-(count - 1))) /
                (std::pow(0.5, static_cast<double>(-count)) - 1.0);
            double amplitude = spec.base_amplitude * amplitude_scale;
            double frequency = std::pow(2.0, static_cast<double>(spec.base_octave));

            double weight_sum = 0.0;
            double deviation_sq = 0.0;
            for (int i = 0; i < count; ++i) {
                const double modifier = spec.amplitude_modifiers.empty()
                    ? 1.0
                    : (i < static_cast<int>(spec.amplitude_modifiers.size())
                        ? spec.amplitude_modifiers[static_cast<std::size_t>(i)] : 1.0);
                if (modifier != 0.0) {
                    Octave octave;
                    octave.perlin.init(mix64(seed + static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull));
                    octave.frequency = frequency;
                    octave.amplitude = amplitude * modifier;
                    octaves_.push_back(octave);
                    weight_sum += std::abs(octave.amplitude);
                    const double layer = 0.2702247831245211 * std::abs(octave.amplitude);
                    deviation_sq += layer * layer;
                }
                frequency *= 2.0;
                amplitude *= 0.5;
            }

            const double deviation = std::sqrt(deviation_sq);
            if (deviation > 0.0) {
                normalization_ = (weight_sum / 3.0) / (deviation * std::sqrt(2.0));
            }
            else {
                normalization_ = 0.0;
            }
        }

        double sample(double x, double y, double z) const {
            double total = 0.0;
            for (const auto& octave : octaves_) {
                total += octave.perlin.sample(x * octave.frequency, y * octave.frequency,
                    z * octave.frequency) * octave.amplitude;
            }
            return total * normalization_;
        }

        bool empty() const noexcept { return octaves_.empty(); }

    private:
        struct Octave {
            Perlin perlin;
            double frequency = 1.0;
            double amplitude = 1.0;
        };

        std::vector<Octave> octaves_;
        double normalization_ = 1.0;
    };

    // The game's noise registry, as dumped to registries/.../worldgen/noise.json.
    class NoiseRegistry {
    public:
        bool load(std::uint32_t world_seed, const nlohmann::json& root) {
            noises_.clear();
            const auto* entries = find_entries(root);
            if (entries == nullptr) {
                return false;
            }

            std::uint32_t index = 0;
            for (const auto& entry : *entries) {
                if (!entry.contains("id") || !entry.contains("data")) {
                    continue;
                }
                const auto& data = entry["data"];
                NoiseSpec spec;
                spec.base_octave = data.value("base_octave", 0);
                spec.base_amplitude = data.value("base_amplitude", 1.0);
                spec.octave_count = data.value("octave_count", 1);
                if (data.contains("amplitude_modifiers") && data["amplitude_modifiers"].is_array()) {
                    for (const auto& value : data["amplitude_modifiers"]) {
                        if (value.is_number()) {
                            spec.amplitude_modifiers.push_back(value.get<double>());
                        }
                    }
                }

                const std::string id = entry["id"].get<std::string>();
                const std::uint64_t seed = mix64(static_cast<std::uint64_t>(world_seed) << 32 ^
                    mix64(static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ull + 0x5EEDull));
                NormalNoise& noise = noises_[id];
                noise.init(spec, seed);
                ++index;
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
