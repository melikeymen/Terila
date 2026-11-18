#pragma once

// The density-function graph the game serialises to worldgen/density_functions.json.
//
// A density function is a small tree of arithmetic and noise nodes: the router names a few
// roots (temperature, vegetation, continents, erosion, depth, ridges, final_density) and the
// terrain is whatever `final_density > 0` says is solid. Evaluating the tree as data keeps the
// world shape the game ships without carrying a line of its code: the node set and the field
// names below are the documented format, the evaluation is ours.

#include "world/worldgen/noise.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    enum class DfKind : std::uint8_t {
        Constant, Y, Zero,
        Add, Mul, Min, Max, Sub, Div, Negate,
        Abs, Square, Cube, HalfNegative, QuarterNegative, Squeeze,
        Clamp, Gradient, Noise, Shift, ShiftA, ShiftB, Cache,
        Spline, RangeChoice, IntervalSelect, Lerp,
        BlendDensity, BlendAlpha, BlendOffset, Beardifier,
        FindTopSurface, OldBlendedNoise, Slice, DistanceToPoint,
    };

    struct DfNode;

    using DfPtr = std::shared_ptr<const DfNode>;

    struct SplinePoint {
        double location = 0.0;
        double derivative = 0.0;
        DfPtr value;
    };

    // The three-fbm blend behind `minecraft:old_blended_noise`: a minimum and a maximum field
    // and a main field whose value chooses between them. The octave shape (16 low octaves for
    // the limits, 8 for the main field, y smearing) is part of the node's definition.
    class BlendedNoise {
    public:
        void init(std::uint64_t seed, double xz_scale, double y_scale, double xz_factor,
            double y_factor, double smear_scale) {
            xz_multiplier_ = 684.412 * xz_scale;
            y_multiplier_ = 684.412 * y_scale;
            xz_factor_ = xz_factor;
            y_factor_ = y_factor;
            const double smear_y = y_multiplier_ * smear_scale;

            build(min_noise_, mix64(seed ^ 0x1E5ull), -15, smear_y, 0.99998474);
            build(max_noise_, mix64(seed ^ 0x2E5ull), -15, smear_y, 0.99998474);
            build(main_noise_, mix64(seed ^ 0x3E5ull), -7, smear_y / y_factor, 12.75);
        }

        double sample(double x, double y, double z) const {
            const double xz = xz_multiplier_;
            const double ym = y_multiplier_;
            const double min_value = sample_stack(min_noise_, x * xz, y * ym, z * xz);
            const double max_value = sample_stack(max_noise_, x * xz, y * ym, z * xz);
            const double main_value = sample_stack(main_noise_, x * xz / xz_factor_,
                y * ym / y_factor_, z * xz / xz_factor_);
            const double t = std::clamp(main_value + 0.5, 0.0, 1.0);
            return min_value + (max_value - min_value) * t;
        }

    private:
        struct Layer {
            Perlin perlin;
            double frequency = 1.0;
            double amplitude = 1.0;
            double smear = 1.0;
        };

        static void build(std::vector<Layer>& out, std::uint64_t seed, int first_octave,
            double smear_y, double value_factor) {
            const int octaves = -first_octave + 1;
            double frequency = 1.0;
            double amplitude = value_factor / (std::pow(2.0, octaves) - 1.0);
            for (int i = octaves - 1; i >= 0; --i) {
                Layer layer;
                layer.perlin.init(mix64(seed + static_cast<std::uint64_t>(i) *
                    0x9E3779B97F4A7C15ull));
                layer.frequency = frequency;
                layer.amplitude = amplitude;
                layer.smear = smear_y * frequency;
                out.push_back(layer);
                frequency /= 2.0;
                amplitude *= 2.0;
            }
        }

        static double sample_stack(const std::vector<Layer>& stack, double x, double y,
            double z) {
            double total = 0.0;
            for (const auto& layer : stack) {
                // The smear makes a layer constant inside a thin horizontal slab, which is
                // what keeps the classic terrain from looking vertically stripy.
                double sample_y = y;
                if (layer.smear > 1.0e-6) {
                    sample_y = std::floor(y / layer.smear + 1.0e-7) * layer.smear;
                }
                total += layer.perlin.sample(x * layer.frequency, sample_y * layer.frequency,
                    z * layer.frequency) * layer.amplitude;
            }
            return total;
        }

        std::vector<Layer> min_noise_;
        std::vector<Layer> max_noise_;
        std::vector<Layer> main_noise_;
        double xz_multiplier_ = 684.412;
        double y_multiplier_ = 684.412;
        double xz_factor_ = 80.0;
        double y_factor_ = 160.0;
    };

    struct DfNode {
        DfKind kind = DfKind::Constant;

        // Scalars, meaning depends on the kind:
        //   Constant -> v0, Gradient -> from/to coordinate + values, Clamp -> bounds,
        //   RangeChoice -> range, Slice -> coordinate, DistanceToPoint -> point.
        double v0 = 0.0;
        double v1 = 0.0;
        double v2 = 0.0;
        double v3 = 0.0;
        int axis = 1;

        DfPtr a;
        DfPtr b;
        DfPtr c;
        std::vector<DfPtr> children;

        std::vector<double> numbers;

        std::vector<SplinePoint> points;

        const NormalNoise* noise = nullptr;
        double xz_scale = 1.0;
        double y_scale = 1.0;
        int cell_height = 8;
        // Index into the per-evaluation cache slots, for `minecraft:cache`.
        int cache_index = -1;

        std::shared_ptr<const BlendedNoise> blended;
    };

    struct EvalContext {
        // A cache node remembers its last position and value. One slot per node, shared by
        // every sample of a chunk, which is what lets the splines ask for the continent field
        // repeatedly without paying for the noise again.
        struct CacheSlot {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            double value = 0.0;
            bool valid = false;
        };

        // Column tops, shared per chunk: material rules ask for the same column many times.
        std::unordered_map<std::uint64_t, double>* column_tops = nullptr;
        std::vector<CacheSlot>* slots = nullptr;
        int recursion = 0;

        void reset(int slot_count) {
            column_tops = nullptr;
            if (slots == nullptr) {
                slots = &owned;
            }
            owned.assign(static_cast<std::size_t>(slot_count), CacheSlot{});
            slots = &owned;
            recursion = 0;
        }

    private:
        std::vector<CacheSlot> owned;
    };

    class DensityGraph {
    public:
        bool load(std::uint32_t world_seed, const nlohmann::json& functions,
            const nlohmann::json& noises) {
            if (!noises_.load(world_seed, noises)) {
                return false;
            }
            raw_.clear();
            resolved_.clear();
            world_seed_ = world_seed;

            const nlohmann::json* entries = entries_of(functions);
            if (entries == nullptr) {
                return false;
            }
            for (const auto& entry : *entries) {
                if (!entry.contains("id") || !entry.contains("data")) {
                    continue;
                }
                raw_[entry["id"].get<std::string>()] = entry["data"];
            }
            return !raw_.empty();
        }

        const NormalNoise* noise(std::string_view id) const { return noises_.find(id); }

        std::size_t noise_count() const noexcept { return noises_.size(); }
        std::size_t function_count() const noexcept { return raw_.size(); }
        int cache_slot_count() const noexcept { return next_cache_index_; }

        DfPtr find(std::string_view id) {
            const std::string key(id);
            if (const auto hit = resolved_.find(key); hit != resolved_.end()) {
                return hit->second;
            }
            if (key == "minecraft:y") {
                auto node = std::make_shared<DfNode>();
                node->kind = DfKind::Y;
                resolved_[key] = node;
                return node;
            }
            if (key == "minecraft:zero") {
                auto node = std::make_shared<DfNode>();
                node->kind = DfKind::Zero;
                resolved_[key] = node;
                return node;
            }
            const auto raw = raw_.find(key);
            if (raw == raw_.end()) {
                return nullptr;
            }
            // Mark before descending: a self-referential definition would otherwise recurse
            // until the stack runs out. A cycle is a broken registry, so the inner reference
            // simply becomes a constant zero.
            const auto placeholder = std::make_shared<DfNode>();
            placeholder->kind = DfKind::Zero;
            resolved_[key] = placeholder;
            const DfPtr parsed = parse(raw->second, 0);
            resolved_[key] = parsed;
            return parsed;
        }

        double sample(const DfPtr& node, double x, double y, double z,
            EvalContext& context) const {
            if (!node) {
                return 0.0;
            }
            return eval(*node, x, y, z, context);
        }

    private:
        static const nlohmann::json* entries_of(const nlohmann::json& root) {
            if (root.contains("data") && root["data"].is_array()) {
                return &root["data"];
            }
            if (root.contains("registries") && root["registries"].contains("entries")) {
                return &root["registries"]["entries"];
            }
            return nullptr;
        }

        DfPtr parse(const nlohmann::json& value, int depth) {
            if (depth > 64) {
                auto zero = std::make_shared<DfNode>();
                zero->kind = DfKind::Zero;
                return zero;
            }
            if (value.is_number()) {
                auto node = std::make_shared<DfNode>();
                node->kind = DfKind::Constant;
                node->v0 = value.get<double>();
                return node;
            }
            if (value.is_string()) {
                const std::string id = value.get<std::string>();
                if (id == "minecraft:y") {
                    auto node = std::make_shared<DfNode>();
                    node->kind = DfKind::Y;
                    return node;
                }
                if (id == "minecraft:zero") {
                    auto node = std::make_shared<DfNode>();
                    node->kind = DfKind::Zero;
                    return node;
                }
                if (const auto hit = resolved_.find(id); hit != resolved_.end() &&
                    hit->second->kind != DfKind::Zero) {
                    return hit->second;
                }
                return find(id);
            }
            if (!value.is_object()) {
                auto zero = std::make_shared<DfNode>();
                zero->kind = DfKind::Zero;
                return zero;
            }

            const std::string type = value.value("type", std::string());
            auto node = std::make_shared<DfNode>();
            node->kind = kind_of(type);
            node->axis = axis_of(value.value("axis", std::string("y")));

            switch (node->kind) {
            case DfKind::Constant:
                node->v0 = value.value("value", 0.0);
                break;
            case DfKind::Add:
            case DfKind::Mul:
            case DfKind::Min:
            case DfKind::Max:
            case DfKind::Sub:
            case DfKind::Div:
                node->a = parse_field(value, "left", depth + 1);
                node->b = parse_field(value, "right", depth + 1);
                break;
            case DfKind::Negate:
            case DfKind::Abs:
            case DfKind::Square:
            case DfKind::Cube:
            case DfKind::HalfNegative:
            case DfKind::QuarterNegative:
            case DfKind::Squeeze:
                break;
            case DfKind::Clamp:
                node->a = parse_field(value, "input", depth + 1);
                node->v0 = value.value("min", 0.0);
                node->v1 = value.value("max", 1.0);
                break;
            case DfKind::Gradient:
                node->v0 = value.value("from_coordinate", 0.0);
                node->v1 = value.value("to_coordinate", 1.0);
                node->v2 = value.value("from_value", 0.0);
                node->v3 = value.value("to_value", 1.0);
                break;
            case DfKind::Noise:
                node->noise = noises_.find(value.value("noise", std::string()));
                node->xz_scale = value.value("xz_scale", 1.0);
                node->y_scale = value.value("y_scale", 1.0);
                if (value.contains("shift_x") && !value["shift_x"].is_null()) {
                    node->a = parse(value["shift_x"], depth + 1);
                }
                if (value.contains("shift_z") && !value["shift_z"].is_null()) {
                    node->b = parse(value["shift_z"], depth + 1);
                }
                break;
            case DfKind::Shift:
            case DfKind::ShiftA:
            case DfKind::ShiftB:
                node->noise = noises_.find(value.value("noise", std::string()));
                break;
            case DfKind::BlendDensity:
                node->a = parse_field(value, "input", depth + 1);
                break;
            case DfKind::Cache:
                node->cache_index = next_cache_index_++;
                break;
            case DfKind::Spline:
                parse_spline(value["spline"], *node, depth + 1);
                break;
            case DfKind::RangeChoice:
                node->a = parse_field(value, "input", depth + 1);
                node->v0 = value.value("min_inclusive", -std::numeric_limits<double>::max());
                node->v1 = value.value("max_exclusive", std::numeric_limits<double>::max());
                node->b = parse_field(value, "when_in_range", depth + 1);
                node->c = parse_field(value, "when_out_of_range", depth + 1);
                break;
            case DfKind::IntervalSelect:
                node->a = parse_field(value, "input", depth + 1);
                if (value.contains("thresholds") && value["thresholds"].is_array()) {
                    for (const auto& threshold : value["thresholds"]) {
                        if (threshold.is_number()) {
                            node->numbers.push_back(threshold.get<double>());
                        }
                    }
                }
                if (value.contains("functions") && value["functions"].is_array()) {
                    for (const auto& function : value["functions"]) {
                        node->children.push_back(parse(function, depth + 1));
                    }
                }
                break;
            case DfKind::Lerp:
                node->a = parse_field(value, "alpha", depth + 1);
                node->b = parse_field(value, "first", depth + 1);
                node->c = parse_field(value, "second", depth + 1);
                break;
            case DfKind::FindTopSurface:
                node->a = parse_field(value, "density", depth + 1);
                node->b = parse_field(value, "upper_bound", depth + 1);
                node->v0 = value.value("lower_bound", -64.0);
                node->cell_height = std::max(1, value.value("cell_height", 8));
                break;
            case DfKind::OldBlendedNoise: {
                auto blended = std::make_shared<BlendedNoise>();
                blended->init(mix64(static_cast<std::uint64_t>(world_seed_) * 0x9E3779B97F4A7C15ull),
                    value.value("xz_scale", 1.0), value.value("y_scale", 1.0),
                    value.value("xz_factor", 80.0), value.value("y_factor", 160.0),
                    value.value("smear_scale_multiplier", 8.0));
                node->blended = blended;
                break;
            }
            case DfKind::Slice:
                node->a = parse_field(value, "input", depth + 1);
                node->v0 = value.value("coordinate", 0.0);
                break;
            case DfKind::DistanceToPoint:
                if (value.contains("point") && value["point"].is_array()) {
                    for (const auto& coordinate : value["point"]) {
                        if (coordinate.is_number()) {
                            node->numbers.push_back(coordinate.get<double>());
                        }
                    }
                }
                node->v0 = value.value("metric", std::string("euclidean")) == "manhattan" ? 1.0 : 0.0;
                break;
            default:
                break;
            }

            // Unary nodes all read `input`; doing it here keeps the switch above readable.
            if (node->a == nullptr && (node->kind == DfKind::Negate || node->kind == DfKind::Abs ||
                node->kind == DfKind::Square || node->kind == DfKind::Cube ||
                node->kind == DfKind::HalfNegative || node->kind == DfKind::QuarterNegative ||
                node->kind == DfKind::Squeeze || node->kind == DfKind::BlendDensity ||
                node->kind == DfKind::Cache)) {
                node->a = parse_field(value, "input", depth + 1);
            }
            return node;
        }

        DfPtr parse_field(const nlohmann::json& object, const char* name, int depth) {
            if (!object.contains(name) || object[name].is_null()) {
                return nullptr;
            }
            return parse(object[name], depth);
        }

        void parse_spline(const nlohmann::json& spline, DfNode& node, int depth) {
            if (!spline.is_object() || depth > 64) {
                return;
            }
            if (spline.contains("coordinate")) {
                node.a = parse(spline["coordinate"], depth + 1);
            }
            if (!spline.contains("points") || !spline["points"].is_array()) {
                return;
            }
            for (const auto& point : spline["points"]) {
                SplinePoint parsed;
                parsed.location = point.value("location", 0.0);
                parsed.derivative = point.value("derivative", 0.0);
                if (point.contains("value")) {
                    parsed.value = parse(point["value"], depth + 1);
                }
                node.points.push_back(std::move(parsed));
            }
        }

        static DfKind kind_of(std::string_view type) {
            if (type == "minecraft:y") return DfKind::Y;
            if (type == "minecraft:add") return DfKind::Add;
            if (type == "minecraft:mul") return DfKind::Mul;
            if (type == "minecraft:min") return DfKind::Min;
            if (type == "minecraft:max") return DfKind::Max;
            if (type == "minecraft:sub") return DfKind::Sub;
            if (type == "minecraft:div") return DfKind::Div;
            if (type == "minecraft:negate") return DfKind::Negate;
            if (type == "minecraft:abs") return DfKind::Abs;
            if (type == "minecraft:square") return DfKind::Square;
            if (type == "minecraft:cube") return DfKind::Cube;
            if (type == "minecraft:half_negative") return DfKind::HalfNegative;
            if (type == "minecraft:quarter_negative") return DfKind::QuarterNegative;
            if (type == "minecraft:squeeze") return DfKind::Squeeze;
            if (type == "minecraft:clamp") return DfKind::Clamp;
            if (type == "minecraft:gradient" || type == "minecraft:y_clamped_gradient") {
                return DfKind::Gradient;
            }
            if (type == "minecraft:noise") return DfKind::Noise;
            if (type == "minecraft:shift") return DfKind::Shift;
            if (type == "minecraft:shift_a") return DfKind::ShiftA;
            if (type == "minecraft:shift_b") return DfKind::ShiftB;
            if (type == "minecraft:spline") return DfKind::Spline;
            if (type == "minecraft:range_choice") return DfKind::RangeChoice;
            if (type == "minecraft:interval_select") return DfKind::IntervalSelect;
            if (type == "minecraft:lerp") return DfKind::Lerp;
            if (type == "minecraft:blend_density") return DfKind::BlendDensity;
            if (type == "minecraft:blend_alpha") return DfKind::BlendAlpha;
            if (type == "minecraft:blend_offset") return DfKind::BlendOffset;
            if (type == "minecraft:beardifier") return DfKind::Beardifier;
            if (type == "minecraft:find_top_surface") return DfKind::FindTopSurface;
            if (type == "minecraft:old_blended_noise") return DfKind::OldBlendedNoise;
            if (type == "minecraft:slice") return DfKind::Slice;
            if (type == "minecraft:distance_to_point") return DfKind::DistanceToPoint;
            if (type == "minecraft:cache") return DfKind::Cache;
            if (type == "minecraft:interpolated") return DfKind::BlendDensity;
            if (type == "minecraft:constant") return DfKind::Constant;
            if (type == "minecraft:zero") return DfKind::Zero;
            return DfKind::Zero;
        }

        static int axis_of(std::string_view axis) {
            if (axis == "x") return 0;
            if (axis == "z") return 2;
            return 1;
        }

        double eval(const DfNode& node, double x, double y, double z,
            EvalContext& context) const {
            switch (node.kind) {
            case DfKind::Constant: return node.v0;
            case DfKind::Y: return y;
            case DfKind::Zero: return 0.0;
            case DfKind::Add: return eval_of(node.a, x, y, z, context) +
                eval_of(node.b, x, y, z, context);
            case DfKind::Mul: return eval_of(node.a, x, y, z, context) *
                eval_of(node.b, x, y, z, context);
            case DfKind::Min: return std::min(eval_of(node.a, x, y, z, context),
                eval_of(node.b, x, y, z, context));
            case DfKind::Max: return std::max(eval_of(node.a, x, y, z, context),
                eval_of(node.b, x, y, z, context));
            case DfKind::Sub: return eval_of(node.a, x, y, z, context) -
                eval_of(node.b, x, y, z, context);
            case DfKind::Div: {
                const double divisor = eval_of(node.b, x, y, z, context);
                return divisor == 0.0 ? 0.0 : eval_of(node.a, x, y, z, context) / divisor;
            }
            case DfKind::Negate: return -eval_of(node.a, x, y, z, context);
            case DfKind::Abs: return std::abs(eval_of(node.a, x, y, z, context));
            case DfKind::Square: {
                const double value = eval_of(node.a, x, y, z, context);
                return value * value;
            }
            case DfKind::Cube: {
                const double value = eval_of(node.a, x, y, z, context);
                return value * value * value;
            }
            case DfKind::HalfNegative: {
                const double value = eval_of(node.a, x, y, z, context);
                return value < 0.0 ? value * 0.5 : value;
            }
            case DfKind::QuarterNegative: {
                const double value = eval_of(node.a, x, y, z, context);
                return value < 0.0 ? value * 0.25 : value;
            }
            case DfKind::Squeeze: {
                const double clamped = std::clamp(eval_of(node.a, x, y, z, context), -1.0, 1.0);
                return clamped / 2.0 - clamped * clamped * clamped / 24.0;
            }
            case DfKind::Clamp:
                return std::clamp(eval_of(node.a, x, y, z, context), node.v0, node.v1);
            case DfKind::Gradient: {
                const double coordinate = node.axis == 0 ? x : (node.axis == 2 ? z : y);
                const double span = node.v1 - node.v0;
                const double t = span == 0.0 ? 0.0 : std::clamp((coordinate - node.v0) / span,
                    0.0, 1.0);
                return node.v2 + (node.v3 - node.v2) * t;
            }
            case DfKind::Noise: {
                if (node.noise == nullptr || node.noise->empty()) {
                    return 0.0;
                }
                const double shift_x = node.a ? eval_of(node.a, x, y, z, context) : 0.0;
                const double shift_z = node.b ? eval_of(node.b, x, y, z, context) : 0.0;
                return node.noise->sample(x * node.xz_scale + shift_x, y * node.y_scale,
                    z * node.xz_scale + shift_z);
            }
            case DfKind::Shift:
                return node.noise == nullptr ? 0.0
                    : node.noise->sample(x * 0.25, y * 0.25, z * 0.25) * 4.0;
            case DfKind::ShiftA:
                return node.noise == nullptr ? 0.0
                    : node.noise->sample(x * 0.25, 0.0, z * 0.25) * 4.0;
            case DfKind::ShiftB:
                return node.noise == nullptr ? 0.0
                    : node.noise->sample(z * 0.25, x * 0.25, 0.0) * 4.0;
            case DfKind::BlendDensity:
            case DfKind::Cache: {
                if (node.kind == DfKind::BlendDensity || node.cache_index < 0 ||
                    context.slots == nullptr ||
                    context.slots->size() <= static_cast<std::size_t>(node.cache_index)) {
                    return eval_of(node.a, x, y, z, context);
                }
                auto& slot = (*context.slots)[static_cast<std::size_t>(node.cache_index)];
                if (slot.valid && slot.x == x && slot.y == y && slot.z == z) {
                    return slot.value;
                }
                const double value = eval_of(node.a, x, y, z, context);
                slot = EvalContext::CacheSlot{ x, y, z, value, true };
                return value;
            }
            case DfKind::BlendAlpha:
            case DfKind::BlendOffset:
            case DfKind::Beardifier:
                return 0.0;
            case DfKind::Spline:
                return spline(node, x, y, z, context);
            case DfKind::RangeChoice: {
                const double value = eval_of(node.a, x, y, z, context);
                const bool inside = value >= node.v0 && value < node.v1;
                return eval_of(inside ? node.b : node.c, x, y, z, context);
            }
            case DfKind::IntervalSelect: {
                const double value = eval_of(node.a, x, y, z, context);
                std::size_t index = 0;
                while (index < node.numbers.size() && value >= node.numbers[index]) {
                    ++index;
                }
                if (index >= node.children.size()) {
                    return 0.0;
                }
                return eval_of(node.children[index], x, y, z, context);
            }
            case DfKind::Lerp: {
                const double alpha = std::clamp(eval_of(node.a, x, y, z, context), 0.0, 1.0);
                const double first = eval_of(node.b, x, y, z, context);
                const double second = eval_of(node.c, x, y, z, context);
                return first + (second - first) * alpha;
            }
            case DfKind::FindTopSurface:
                return top_surface(node, x, z, context);
            case DfKind::OldBlendedNoise:
                return node.blended ? node.blended->sample(x, y, z) : 0.0;
            case DfKind::Slice: {
                const double coordinate = node.v0;
                return eval_of(node.a,
                    node.axis == 0 ? coordinate : x,
                    node.axis == 1 ? coordinate : y,
                    node.axis == 2 ? coordinate : z, context);
            }
            case DfKind::DistanceToPoint: {
                const double px = node.numbers.size() > 0 ? node.numbers[0] : 0.0;
                const double py = node.numbers.size() > 1 ? node.numbers[1] : 0.0;
                const double pz = node.numbers.size() > 2 ? node.numbers[2] : 0.0;
                const double dx = x - px;
                const double dy = y - py;
                const double dz = z - pz;
                if (node.v0 != 0.0) {
                    return std::abs(dx) + std::abs(dy) + std::abs(dz);
                }
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            default:
                return 0.0;
            }
        }

        double eval_of(const DfPtr& node, double x, double y, double z,
            EvalContext& context) const {
            return node ? eval(*node, x, y, z, context) : 0.0;
        }

        // Piecewise cubic Hermite through the control points, extended linearly past the
        // ends: the shape the game's splines are built from.
        double spline(const DfNode& node, double x, double y, double z,
            EvalContext& context) const {
            if (node.points.empty()) {
                return 0.0;
            }
            const double input = eval_of(node.a, x, y, z, context);

            if (input <= node.points.front().location) {
                const auto& point = node.points.front();
                const double value = eval_of(point.value, x, y, z, context);
                return value + point.derivative * (input - point.location);
            }
            if (input >= node.points.back().location) {
                const auto& point = node.points.back();
                const double value = eval_of(point.value, x, y, z, context);
                return value + point.derivative * (input - point.location);
            }

            std::size_t index = 0;
            while (index + 1 < node.points.size() &&
                input >= node.points[index + 1].location) {
                ++index;
            }
            const auto& first = node.points[index];
            const auto& second = node.points[index + 1];
            const double span = second.location - first.location;
            if (span <= 0.0) {
                return eval_of(first.value, x, y, z, context);
            }
            const double t = (input - first.location) / span;
            const double y1 = eval_of(first.value, x, y, z, context);
            const double y2 = eval_of(second.value, x, y, z, context);
            const double a = first.derivative * span - (y2 - y1);
            const double b = -second.derivative * span + (y2 - y1);
            return (y1 + (y2 - y1) * t) + t * (1.0 - t) * (a + (b - a) * t);
        }

        // The top of the noise surface: walk down in cells while the density is positive and
        // interpolate the crossing. Material rules ask this per block, so the answer is kept
        // per column.
        double top_surface(const DfNode& node, double x, double z, EvalContext& context) const {
            const std::uint64_t key = (static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(x)))) << 32) |
                static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(z)));
            if (context.column_tops != nullptr) {
                if (const auto hit = context.column_tops->find(key);
                    hit != context.column_tops->end()) {
                    return hit->second;
                }
            }
            if (context.recursion > 0) {
                return node.v0;
            }

            ++context.recursion;
            const double upper = node.b ? eval_of(node.b, x, 0.0, z, context) : 320.0;
            const double lower = node.v0;
            double result = lower;
            for (double at = std::floor(upper); at >= lower; at -= node.cell_height) {
                const double density = eval_of(node.a, x, at, z, context);
                if (density > 0.0) {
                    const double above = eval_of(node.a, x, at + node.cell_height, z, context);
                    const double t = above > density ? 0.0
                        : density / (density - above);
                    result = at + std::clamp(t, 0.0, 1.0) * node.cell_height;
                    break;
                }
            }
            --context.recursion;

            if (context.column_tops != nullptr) {
                (*context.column_tops)[key] = result;
            }
            return result;
        }

        NoiseRegistry noises_;
        std::unordered_map<std::string, nlohmann::json> raw_;
        mutable         std::unordered_map<std::string, DfPtr> resolved_;
        int next_cache_index_ = 0;
        std::uint32_t world_seed_ = 0;
    };

} // namespace terila::world::worldgen
