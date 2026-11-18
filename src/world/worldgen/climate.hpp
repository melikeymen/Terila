#pragma once

// The multi-noise biome source: the game describes every overworld biome as a box in a
// six-axis climate space (temperature, humidity, continentalness, erosion, depth, weirdness)
// and picks the biome of the first box that contains the sampled point.
//
// The parameter table is large (thousands of boxes), so points are also bucketed along the
// two cheapest axes and a lookup only scans the buckets the point falls into.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    struct ClimateRange {
        double min = -1.0;
        double max = 1.0;

        bool contains(double value) const noexcept {
            return value >= min && value <= max;
        }
    };

    struct BiomePoint {
        std::array<ClimateRange, 6> climate{};
        std::string biome;
    };

    class BiomeParameters {
    public:
        bool load(const nlohmann::json& root, std::string_view preset) {
            points_.clear();
            buckets_.clear();

            const nlohmann::json* values = preset_values(root, preset);
            if (values == nullptr || !values->contains("biomes") || !(*values)["biomes"].is_array()) {
                return false;
            }

            for (const auto& entry : (*values)["biomes"]) {
                if (!entry.contains("biome") || !entry.contains("parameters")) {
                    continue;
                }
                const auto& parameters = entry["parameters"];
                static constexpr const char* kAxes[6] = {
                    "temperature", "humidity", "continentalness", "erosion", "depth",
                    "weirdness"
                };
                BiomePoint point;
                point.biome = entry["biome"].get<std::string>();
                for (std::size_t axis = 0; axis < 6; ++axis) {
                    const auto found = parameters.find(kAxes[axis]);
                    point.climate[axis] = found == parameters.end()
                        ? ClimateRange{ -1.0, 1.0 }
                        : range_of(*found);
                }
                points_.push_back(std::move(point));
            }
            if (points_.empty()) {
                return false;
            }

            buckets_.assign(static_cast<std::size_t>(buckets_per_axis_ * buckets_per_axis_),
                std::vector<std::uint32_t>{});
            for (std::size_t index = 0; index < points_.size(); ++index) {
                const auto& point = points_[index];
                const int first_x = bucket_of(point.climate[0].min);
                const int last_x = bucket_of(point.climate[0].max);
                const int first_z = bucket_of(point.climate[1].min);
                const int last_z = bucket_of(point.climate[1].max);
                for (int bx = first_x; bx <= last_x; ++bx) {
                    for (int bz = first_z; bz <= last_z; ++bz) {
                        buckets_[static_cast<std::size_t>(bx * buckets_per_axis_ + bz)]
                            .push_back(static_cast<std::uint32_t>(index));
                    }
                }
            }
            return true;
        }

        // `climate` is temperature, humidity, continentalness, erosion, depth, weirdness.
        // Null when no box contains the point.
        const std::string* find(const std::array<double, 6>& climate) const {
            const auto& bucket = buckets_[static_cast<std::size_t>(
                bucket_of(climate[0]) * buckets_per_axis_ + bucket_of(climate[1]))];
            for (const auto index : bucket) {
                const auto& point = points_[index];
                bool inside = true;
                for (std::size_t axis = 0; axis < 6; ++axis) {
                    if (!point.climate[axis].contains(climate[axis])) {
                        inside = false;
                        break;
                    }
                }
                if (inside) {
                    return &point.biome;
                }
            }
            return nullptr;
        }

        std::size_t size() const noexcept { return points_.size(); }
        std::size_t bucket_count() const noexcept { return buckets_.size(); }

    private:
        static const nlohmann::json* preset_values(const nlohmann::json& root,
            std::string_view preset) {
            if (!root.contains("data") || !root["data"].is_object()) {
                return nullptr;
            }
            const auto& data = root["data"];
            if (const auto exact = data.find(std::string(preset));
                exact != data.end() && exact->contains("values")) {
                return &(*exact)["values"];
            }
            for (const auto& entry : data.items()) {
                if (entry.value().contains("values")) {
                    return &entry.value()["values"];
                }
            }
            return nullptr;
        }

        static ClimateRange range_of(const nlohmann::json& value) {
            ClimateRange range;
            if (value.is_array() && value.size() == 2) {
                range.min = value[0].get<double>();
                range.max = value[1].get<double>();
            }
            else if (value.is_number()) {
                range.min = value.get<double>();
                range.max = range.min;
            }
            return range;
        }

        int bucket_of(double value) const {
            const double normalized = (value - bucket_min_) / bucket_span_;
            const int index = static_cast<int>(std::floor(normalized *
                static_cast<double>(buckets_per_axis_)));
            return std::clamp(index, 0, buckets_per_axis_ - 1);
        }

        std::vector<BiomePoint> points_;
        std::vector<std::vector<std::uint32_t>> buckets_;
        // The width covers the whole climate range with a margin: a box that spills past the
        // edge is still found by the clamped bucket.
        int buckets_per_axis_ = 24;
        double bucket_min_ = -1.25;
        double bucket_span_ = 2.5;
    };

} // namespace terila::world::worldgen
