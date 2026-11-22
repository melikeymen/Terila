#pragma once

// The density graph as plain arrays.
//
// A device cannot walk shared pointers, so the tree behind `final_density` is flattened into
// node, child, spline-point and noise tables that a kernel indexes by integer. The plan is
// built once per generator and read-only afterwards, which is what lets several chunk workers
// hand their grid to the device at the same time.
//
// A node the device path does not carry leaves the plan unusable instead of producing a
// different field: the caller keeps the CPU path in that case.

#include "world/worldgen/density/density.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace terila::world::worldgen::gpu {

    struct PlanNode {
        DfKind kind = DfKind::Constant;
        int axis = 1;
        int a = -1;
        int b = -1;
        int c = -1;
        int noise = -1;
        int blended = -1;
        int cell_xz = 4;
        int cell_height = 8;
        int first_child = 0;
        int child_count = 0;
        int first_number = 0;
        int number_count = 0;
        int first_point = 0;
        int point_count = 0;
        double v0 = 0.0;
        double v1 = 0.0;
        double v2 = 0.0;
        double v3 = 0.0;
        double xz_scale = 1.0;
        double y_scale = 1.0;
    };

    struct PlanPoint {
        double location = 0.0;
        double derivative = 0.0;
        int value = -1;
    };

    // One Perlin octave: the lattice table it permutes with and the three offsets it is
    // shifted by, plus the weight the stack gives it.
    struct PlanLayer {
        std::size_t first_perm = 0;
        double offset_x = 0.0;
        double offset_y = 0.0;
        double offset_z = 0.0;
        double frequency = 1.0;
        double smear = 1.0;
        float amplitude = 1.0f;
    };

    struct PlanNoise {
        int first_layer = 0;
        int layer_count = 0;
    };

    // The three stacks of `minecraft:old_blended_noise`.
    struct PlanBlended {
        int first_min = 0;
        int count_min = 0;
        int first_max = 0;
        int count_max = 0;
        int first_main = 0;
        int count_main = 0;
        double xz_multiplier = 684.412;
        double y_multiplier = 684.412;
        double xz_factor = 80.0;
        double y_factor = 160.0;
    };

    struct DensityPlan {
        // Identifies the plan for the device upload cache: two plans can live at the same
        // address one after the other, and their tables must not be confused.
        std::uint64_t id = 0;
        std::vector<PlanNode> nodes;
        std::vector<int> children;
        std::vector<double> numbers;
        std::vector<PlanPoint> points;
        std::vector<PlanLayer> layers;
        std::vector<unsigned char> permutations;
        std::vector<PlanNoise> noises;
        std::vector<PlanBlended> blended;
        int root = -1;
        // Why the device cannot evaluate this graph, empty when it can.
        std::string unsupported;

        bool usable() const noexcept { return root >= 0 && unsupported.empty(); }
    };

    namespace detail {

        inline const char* node_kind_name(DfKind kind) {
            switch (kind) {
            case DfKind::Constant: return "minecraft:constant";
            case DfKind::Y: return "minecraft:y";
            case DfKind::Zero: return "minecraft:zero";
            case DfKind::Add: return "minecraft:add";
            case DfKind::Mul: return "minecraft:mul";
            case DfKind::Min: return "minecraft:min";
            case DfKind::Max: return "minecraft:max";
            case DfKind::Sub: return "minecraft:sub";
            case DfKind::Div: return "minecraft:div";
            case DfKind::Negate: return "minecraft:negate";
            case DfKind::Abs: return "minecraft:abs";
            case DfKind::Square: return "minecraft:square";
            case DfKind::Cube: return "minecraft:cube";
            case DfKind::HalfNegative: return "minecraft:half_negative";
            case DfKind::QuarterNegative: return "minecraft:quarter_negative";
            case DfKind::Squeeze: return "minecraft:squeeze";
            case DfKind::Clamp: return "minecraft:clamp";
            case DfKind::Gradient: return "minecraft:gradient";
            case DfKind::Noise: return "minecraft:noise";
            case DfKind::Shift: return "minecraft:shift";
            case DfKind::ShiftA: return "minecraft:shift_a";
            case DfKind::ShiftB: return "minecraft:shift_b";
            case DfKind::Cache: return "minecraft:cache";
            case DfKind::Spline: return "minecraft:spline";
            case DfKind::RangeChoice: return "minecraft:range_choice";
            case DfKind::IntervalSelect: return "minecraft:interval_select";
            case DfKind::Lerp: return "minecraft:lerp";
            case DfKind::Interpolated: return "minecraft:interpolated";
            case DfKind::BlendDensity: return "minecraft:blend_density";
            case DfKind::BlendAlpha: return "minecraft:blend_alpha";
            case DfKind::BlendOffset: return "minecraft:blend_offset";
            case DfKind::Beardifier: return "minecraft:beardifier";
            case DfKind::FindTopSurface: return "minecraft:find_top_surface";
            case DfKind::OldBlendedNoise: return "minecraft:old_blended_noise";
            case DfKind::Slice: return "minecraft:slice";
            case DfKind::DistanceToPoint: return "minecraft:distance_to_point";
            }
            return "minecraft:unknown";
        }

        class Planner {
        public:
            DensityPlan plan;

            int add(const DfPtr& node) {
                if (!node) {
                    return -1;
                }
                if (const auto hit = ids_.find(node.get()); hit != ids_.end()) {
                    return hit->second;
                }
                const int index = static_cast<int>(plan.nodes.size());
                ids_.emplace(node.get(), index);
                plan.nodes.emplace_back();

                PlanNode built;
                built.kind = node->kind;
                built.axis = node->axis;
                built.v0 = node->v0;
                built.v1 = node->v1;
                built.v2 = node->v2;
                built.v3 = node->v3;
                built.cell_xz = node->cell_xz;
                built.cell_height = node->cell_height;
                built.xz_scale = node->xz_scale;
                built.y_scale = node->y_scale;

                if (node->kind == DfKind::FindTopSurface) {
                    if (plan.unsupported.empty()) {
                        plan.unsupported = node_kind_name(node->kind);
                    }
                }
                if (node->kind == DfKind::Noise || node->kind == DfKind::Shift ||
                    node->kind == DfKind::ShiftA || node->kind == DfKind::ShiftB) {
                    built.noise = noise_index(node->noise);
                }
                if (node->kind == DfKind::OldBlendedNoise) {
                    built.blended = blended_index(node->blended.get());
                }
                if (!node->numbers.empty()) {
                    built.first_number = static_cast<int>(plan.numbers.size());
                    built.number_count = static_cast<int>(node->numbers.size());
                    plan.numbers.insert(plan.numbers.end(), node->numbers.begin(),
                        node->numbers.end());
                }
                if (!node->children.empty()) {
                    built.first_child = static_cast<int>(plan.children.size());
                    built.child_count = static_cast<int>(node->children.size());
                    plan.children.resize(plan.children.size() + node->children.size(), -1);
                }
                if (!node->points.empty()) {
                    built.first_point = static_cast<int>(plan.points.size());
                    built.point_count = static_cast<int>(node->points.size());
                    plan.points.resize(plan.points.size() + node->points.size());
                }

                // The children are resolved after the tables above, so a reallocation of the
                // node list cannot leave this node's slot pointing at freed memory.
                built.a = add(node->a);
                built.b = add(node->b);
                built.c = add(node->c);
                for (std::size_t i = 0; i < node->children.size(); ++i) {
                    plan.children[static_cast<std::size_t>(built.first_child) + i] =
                        add(node->children[i]);
                }
                for (std::size_t i = 0; i < node->points.size(); ++i) {
                    PlanPoint point;
                    point.location = node->points[i].location;
                    point.derivative = node->points[i].derivative;
                    plan.points[static_cast<std::size_t>(built.first_point) + i] = point;
                    const int value = add(node->points[i].value);
                    plan.points[static_cast<std::size_t>(built.first_point) + i].value = value;
                }
                plan.nodes[static_cast<std::size_t>(index)] = built;
                return index;
            }

        private:
            int noise_index(const NormalNoise* noise) {
                if (noise == nullptr) {
                    return -1;
                }
                if (const auto hit = noise_ids_.find(noise); hit != noise_ids_.end()) {
                    return hit->second;
                }
                const int index = static_cast<int>(plan.noises.size());
                noise_ids_.emplace(noise, index);
                PlanNoise entry;
                entry.first_layer = static_cast<int>(plan.layers.size());
                for (const auto& layer : noise->layers()) {
                    push_layer(layer.noise, layer.frequency, layer.amplitude, 1.0);
                }
                entry.layer_count = static_cast<int>(plan.layers.size()) - entry.first_layer;
                plan.noises.push_back(entry);
                return index;
            }

            void push_layer(const Perlin& noise, double frequency, float amplitude,
                double smear) {
                PlanLayer layer;
                layer.first_perm = plan.permutations.size();
                const auto& table = noise.permutations();
                plan.permutations.insert(plan.permutations.end(), table.begin(), table.end());
                layer.offset_x = noise.offset_x();
                layer.offset_y = noise.offset_y();
                layer.offset_z = noise.offset_z();
                layer.frequency = frequency;
                layer.smear = smear;
                layer.amplitude = amplitude;
                plan.layers.push_back(layer);
            }

            int blended_index(const BlendedNoise* noise) {
                if (noise == nullptr) {
                    return -1;
                }
                if (const auto hit = blended_ids_.find(noise); hit != blended_ids_.end()) {
                    return hit->second;
                }
                const int index = static_cast<int>(plan.blended.size());
                blended_ids_.emplace(noise, index);
                PlanBlended entry;
                entry.xz_multiplier = noise->xz_multiplier();
                entry.y_multiplier = noise->y_multiplier();
                entry.xz_factor = noise->xz_factor();
                entry.y_factor = noise->y_factor();
                entry.first_min = push_stack(noise->min_layers());
                entry.count_min = static_cast<int>(plan.layers.size()) - entry.first_min;
                entry.first_max = push_stack(noise->max_layers());
                entry.count_max = static_cast<int>(plan.layers.size()) - entry.first_max;
                entry.first_main = push_stack(noise->main_layers());
                entry.count_main = static_cast<int>(plan.layers.size()) - entry.first_main;
                plan.blended.push_back(entry);
                return index;
            }

            int push_stack(const std::vector<BlendedNoise::Layer>& stack) {
                const int first = static_cast<int>(plan.layers.size());
                for (const auto& layer : stack) {
                    push_layer(layer.perlin, layer.frequency,
                        static_cast<float>(layer.amplitude), layer.smear);
                }
                return first;
            }

            std::unordered_map<const DfNode*, int> ids_;
            std::unordered_map<const NormalNoise*, int> noise_ids_;
            std::unordered_map<const BlendedNoise*, int> blended_ids_;
        };

    } // namespace detail

    inline std::uint64_t next_plan_id() {
        static std::atomic<std::uint64_t> next{ 0 };
        return ++next;
    }

    inline DensityPlan build_plan(const DfPtr& root) {
        detail::Planner planner;
        planner.plan.id = next_plan_id();
        planner.plan.root = planner.add(root);
        if (planner.plan.root < 0 && planner.plan.unsupported.empty()) {
            planner.plan.unsupported = "no final_density";
        }
        return planner.plan;
    }

} // namespace terila::world::worldgen::gpu
