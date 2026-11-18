#pragma once

// The material rule chain: the game paints the surface with a small rule language
// (condition -> rule, sequence, block) instead of a per-biome table. `minecraft:overworld`
// starts at the world floor (bedrock gradient), runs the surface rules above the noise
// surface, and finishes with the deepslate gradient; everything here evaluates that data.
//
// The rules that only matter for structures or ore veins are evaluated as "no material",
// which is exactly what the chain does when its condition does not hold.

#include "world/worldgen/density.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    // What a rule can ask about the column it is painting.
    struct MaterialContext {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        int surface_y = 0;        // top solid block of the column
        int stone_above = 0;      // solid blocks between the surface and this cell
        int stone_below = 1 << 20;
        int water_height = std::numeric_limits<int>::min();   // first water block
        int surface_depth = 0;      // noise driven thickness of the surface layer
        double surface_secondary = 0.0;
        double temperature = 0.0;
        int biome = -1;
        double steepness = 0.0;
    };

    class MaterialRules {
    public:
        using BlockResolver = std::function<std::optional<std::int32_t>(std::string_view)>;
        using BiomeResolver = std::function<std::optional<std::int32_t>(std::string_view)>;

        bool load(const nlohmann::json& rules, const nlohmann::json& conditions,
            const DensityGraph& density, BlockResolver block, BiomeResolver biome,
            std::string_view root_id = "minecraft:overworld", int min_y = -64) {
            return load_impl(rules, conditions, density, std::move(block), std::move(biome),
                root_id, min_y);
        }

        bool ready() const noexcept { return root_ != nullptr; }

        // -1 means "this column keeps the block the noise fill gave it".
        std::int32_t apply(const MaterialContext& context) const {
            return root_ ? eval(*root_, context) : -1;
        }

        std::size_t rule_count() const noexcept { return rules_.size(); }

    private:
        enum class Kind : std::uint8_t {
            Block, Sequence, Condition, Bandlands, Nothing,
            ConstantTrue, ConstantFalse,
            Biome, VerticalGradient, NoiseThreshold, StoneDepth, Water, YAbove, Not, Steep,
            AbovePreliminarySurface, Hole, Temperature,
        };

        struct Node;

        using NodePtr = std::shared_ptr<const Node>;

        struct Node {
            Kind kind = Kind::ConstantFalse;
            std::vector<NodePtr> children;
            NodePtr predicate;
            NodePtr then_run;
            std::int32_t state = -1;
            std::vector<std::int32_t> states;
            int biome = -1;
            std::vector<int> biome_list;
            int v0 = 0;
            int v1 = 0;
            int v2 = 0;
            double f0 = 0.0;
            double f1 = 0.0;
            bool flag = false;
            std::uint64_t random_seed = 0;
            const NormalNoise* noise = nullptr;
        };

        static const nlohmann::json* entries_of(const nlohmann::json& root) {
            if (root.contains("registries") && root["registries"].contains("entries")) {
                return &root["registries"]["entries"];
            }
            if (root.contains("data") && root["data"].is_array()) {
                return &root["data"];
            }
            return nullptr;
        }

        static int anchor_of(const nlohmann::json& value, int min_y) {
            if (value.is_object()) {
                if (value.contains("absolute") && value["absolute"].is_number()) {
                    return value["absolute"].get<int>();
                }
                if (value.contains("above_bottom") && value["above_bottom"].is_number()) {
                    return min_y + value["above_bottom"].get<int>();
                }
            }
            if (value.is_number()) {
                return value.get<int>();
            }
            return 0;
        }

        bool load_registry(const nlohmann::json& root,
            std::unordered_map<std::string, nlohmann::json>& out) {
            const auto* entries = entries_of(root);
            if (entries == nullptr) {
                return false;
            }
            for (const auto& entry : *entries) {
                if (entry.contains("id") && entry.contains("data")) {
                    out[entry["id"].get<std::string>()] = entry["data"];
                }
            }
            return true;
        }

        bool load_impl(const nlohmann::json& rules, const nlohmann::json& conditions,
            const DensityGraph& density, BlockResolver block, BiomeResolver biome,
            std::string_view root_id, int min_y) {
            block_ = std::move(block);
            biome_ = std::move(biome);
            density_ = &density;
            min_y_ = min_y;
            if (!load_registry(rules, rules_raw_) || !load_registry(conditions, conditions_raw_)) {
                return false;
            }
            root_ = rule_ref(std::string(root_id), 0);
            return root_ != nullptr;
        }

        NodePtr rule_ref(const std::string& id, int depth) {
            if (depth > 48) {
                return nothing();
            }
            if (const auto hit = rules_.find(id); hit != rules_.end()) {
                return hit->second;
            }
            const auto raw = rules_raw_.find(id);
            if (raw == rules_raw_.end()) {
                return nothing();
            }
            // A registered rule is inserted before its children are parsed so a self reference
            // cannot recurse forever.
            rules_[id] = nothing();
            NodePtr parsed = parse_rule(raw->second, depth + 1);
            if (!parsed) {
                parsed = nothing();
            }
            rules_[id] = parsed;
            return parsed;
        }

        NodePtr condition_ref(const nlohmann::json& value, int depth) {
            if (value.is_string()) {
                const std::string id = value.get<std::string>();
                if (const auto hit = conditions_.find(id); hit != conditions_.end()) {
                    return hit->second;
                }
                const auto raw = conditions_raw_.find(id);
                if (raw == conditions_raw_.end()) {
                    return constant(false);
                }
                conditions_[id] = constant(false);
                NodePtr parsed = parse_condition(raw->second, depth + 1);
                conditions_[id] = parsed;
                return parsed;
            }
            return parse_condition(value, depth + 1);
        }

        NodePtr parse_rule(const nlohmann::json& value, int depth) {
            if (depth > 48) {
                return nothing();
            }
            if (value.is_string()) {
                return rule_ref(value.get<std::string>(), depth);
            }
            if (!value.is_object()) {
                return nothing();
            }

            const std::string type = value.value("type", std::string("minecraft:block"));
            if (type == "minecraft:block") {
                auto node = std::make_shared<Node>();
                node->kind = Kind::Block;
                node->state = resolve_block(value.value("result_state", std::string()));
                return node->state < 0 ? nothing() : node;
            }
            if (type == "minecraft:sequence") {
                auto node = std::make_shared<Node>();
                node->kind = Kind::Sequence;
                if (value.contains("sequence") && value["sequence"].is_array()) {
                    for (const auto& child : value["sequence"]) {
                        node->children.push_back(parse_rule(child, depth + 1));
                    }
                }
                return node;
            }
            if (type == "minecraft:condition") {
                auto node = std::make_shared<Node>();
                node->kind = Kind::Condition;
                if (value.contains("if_true")) {
                    node->predicate = condition_ref(value["if_true"], depth + 1);
                }
                if (value.contains("then_run")) {
                    node->then_run = parse_rule(value["then_run"], depth + 1);
                }
                if (!node->predicate) {
                    return nothing();
                }
                return node;
            }
            if (type == "minecraft:bandlands") {
                auto node = std::make_shared<Node>();
                node->kind = Kind::Bandlands;
                for (const char* name : { "minecraft:terracotta", "minecraft:orange_terracotta",
                    "minecraft:yellow_terracotta", "minecraft:brown_terracotta",
                    "minecraft:white_terracotta", "minecraft:light_gray_terracotta" }) {
                    const std::int32_t state = resolve_block(name);
                    if (state >= 0) {
                        node->states.push_back(state);
                    }
                }
                return node->states.empty() ? nothing() : node;
            }
            if (type == "minecraft:ore_vein") {
                // Ore veins are their own feature in this implementation; the surface chain
                // has nothing to say about them.
                return nothing();
            }
            return nothing();
        }

        NodePtr parse_condition(const nlohmann::json& value, int depth) {
            if (depth > 48) {
                return constant(false);
            }
            if (value.is_string()) {
                return condition_ref(value, depth);
            }
            if (!value.is_object()) {
                return constant(false);
            }

            const std::string type = value.value("type", std::string());
            auto node = std::make_shared<Node>();

            if (type == "minecraft:vertical_gradient") {
                node->kind = Kind::VerticalGradient;
                node->v0 = anchor_of(value.value("true_at_and_below", nlohmann::json{}), min_y_);
                node->v1 = anchor_of(value.value("false_at_and_above", nlohmann::json{}), min_y_);
                node->random_seed = random_seed_of(value.value("random_name", std::string()));
                return node;
            }
            if (type == "minecraft:biome") {
                node->kind = Kind::Biome;
                const auto add = [&](const std::string& name) {
                    const auto resolved = biome_ ? biome_(name) : std::nullopt;
                    if (resolved) {
                        node->biome_list.push_back(*resolved);
                    }
                };
                // A newer rule may name a whole group of biomes instead of one.
                const auto raw = value.find("biome_is");
                if (raw != value.end()) {
                    if (raw->is_string()) {
                        add(raw->get<std::string>());
                    }
                    else if (raw->is_array()) {
                        for (const auto& name : *raw) {
                            if (name.is_string()) {
                                add(name.get<std::string>());
                            }
                        }
                    }
                }
                return node->biome_list.empty() ? constant(false) : node;
            }
            if (type == "minecraft:noise_threshold") {
                node->kind = Kind::NoiseThreshold;
                node->noise = density_ ? density_->noise(value.value("noise", std::string())) : nullptr;
                node->f0 = value.value("min_threshold", -1.0e300);
                node->f1 = value.value("max_threshold", 1.0e300);
                return node->noise == nullptr ? constant(false) : node;
            }
            if (type == "minecraft:stone_depth") {
                node->kind = Kind::StoneDepth;
                node->v0 = value.value("offset", 0);
                node->flag = value.value("add_surface_depth", false);
                node->v1 = value.value("secondary_depth_range", 0);
                node->v2 = value.value("surface_type", std::string("floor")) == "ceiling" ? 1 : 0;
                return node;
            }
            if (type == "minecraft:water") {
                node->kind = Kind::Water;
                node->v0 = value.value("offset", 0);
                node->flag = value.value("add_stone_depth", false);
                node->v1 = value.value("surface_depth_multiplier", 0);
                return node;
            }
            if (type == "minecraft:y_above") {
                node->kind = Kind::YAbove;
                node->v0 = anchor_of(value.value("anchor", nlohmann::json{}), min_y_);
                node->flag = value.value("add_stone_depth", false);
                node->v1 = value.value("surface_depth_multiplier", 0);
                return node;
            }
            if (type == "minecraft:not") {
                node->kind = Kind::Not;
                if (value.contains("invert")) {
                    node->predicate = condition_ref(value["invert"], depth + 1);
                }
                return node->predicate ? node : constant(false);
            }
            if (type == "minecraft:steep") {
                node->kind = Kind::Steep;
                return node;
            }
            if (type == "minecraft:above_preliminary_surface") {
                node->kind = Kind::AbovePreliminarySurface;
                return node;
            }
            if (type == "minecraft:hole") {
                node->kind = Kind::Hole;
                return node;
            }
            if (type == "minecraft:temperature") {
                node->kind = Kind::Temperature;
                node->f0 = value.value("min", -0.45);
                node->f1 = value.value("max", 0.15);
                return node;
            }
            if (type == "minecraft:constant") {
                return constant(value.value("value", false));
            }
            return constant(false);
        }

        std::int32_t resolve_block(const std::string& name) const {
            if (name.empty() || !block_) {
                return -1;
            }
            const auto resolved = block_(name);
            return resolved ? *resolved : -1;
        }

        static std::uint64_t random_seed_of(const std::string& name) {
            std::uint64_t hash = 0xCBF29CE484222325ull;
            for (const char c : name) {
                hash ^= static_cast<std::uint8_t>(c);
                hash *= 0x100000001B3ull;
            }
            return hash;
        }

        static NodePtr nothing() {
            static const NodePtr node = [] {
                auto value = std::make_shared<Node>();
                value->kind = Kind::Nothing;
                return value;
                }();
            return node;
        }

        static NodePtr constant(bool value) {
            static const NodePtr truth = [] {
                auto node = std::make_shared<Node>();
                node->kind = Kind::ConstantTrue;
                return node;
                }();
            static const NodePtr falsity = [] {
                auto node = std::make_shared<Node>();
                node->kind = Kind::ConstantFalse;
                return node;
                }();
            return value ? truth : falsity;
        }

        std::int32_t eval(const Node& node, const MaterialContext& context) const {
            switch (node.kind) {
            case Kind::Block: return node.state;
            case Kind::Nothing: return -1;
            case Kind::Sequence: {
                for (const auto& child : node.children) {
                    if (child) {
                        const std::int32_t state = eval(*child, context);
                        if (state >= 0) {
                            return state;
                        }
                    }
                }
                return -1;
            }
            case Kind::Condition:
                return test(*node.predicate, context) && node.then_run
                    ? eval(*node.then_run, context) : -1;
            case Kind::Bandlands: {
                if (node.states.empty()) {
                    return -1;
                }
                const std::uint64_t hash = static_cast<std::uint64_t>(context.y) * 0x9E3779B97F4A7C15ull +
                    static_cast<std::uint64_t>(context.x) * 0xC2B2AE3D27D4EB4Full;
                return node.states[static_cast<std::size_t>((hash >> 33) % node.states.size())];
            }
            default: return -1;
            }
        }

        bool test(const Node& node, const MaterialContext& context) const {
            switch (node.kind) {
            case Kind::ConstantTrue: return true;
            case Kind::ConstantFalse: return false;
            case Kind::Nothing: return false;
            case Kind::Not: return node.predicate ? !test(*node.predicate, context) : false;
            case Kind::Biome: {
                for (const int id : node.biome_list) {
                    if (id == context.biome) {
                        return true;
                    }
                }
                return false;
            }
            case Kind::VerticalGradient: {
                const int low = std::min(node.v0, node.v1);
                const int high = std::max(node.v0, node.v1);
                if (context.y <= low) {
                    return true;
                }
                if (context.y >= high) {
                    return false;
                }
                const double t = static_cast<double>(context.y - low) /
                    static_cast<double>(high - low);
                const std::uint64_t hash = mix64(node.random_seed ^
                    (static_cast<std::uint64_t>(context.x) * 0x9E3779B97F4A7C15ull) ^
                    (static_cast<std::uint64_t>(context.y) * 0xC2B2AE3D27D4EB4Full) ^
                    static_cast<std::uint64_t>(context.z));
                return static_cast<double>((hash >> 11) % 1000) / 1000.0 < 1.0 - t;
            }
            case Kind::NoiseThreshold: {
                if (node.noise == nullptr) {
                    return false;
                }
                // Surface noise is a two dimensional field sampled at the column origin.
                const double value = node.noise->sample(context.x, 0.0, context.z);
                return value >= node.f0 && value < node.f1;
            }
            case Kind::StoneDepth: {
                const int depth = node.v2 != 0 ? context.stone_below : context.stone_above;
                const int surface = node.flag ? context.surface_depth : 0;
                const int secondary = node.v1 == 0 ? 0 : static_cast<int>(
                    (context.surface_secondary + 1.0) * 0.5 * node.v1);
                return depth <= 1 + node.v0 + surface + secondary;
            }
            case Kind::Water:
                return context.water_height == std::numeric_limits<int>::min() ||
                    context.y + (node.flag ? context.stone_above : 0) >=
                    context.water_height + node.v0 +
                    context.surface_depth * node.v1;
            case Kind::YAbove:
                return context.y + (node.flag ? context.stone_above : 0) >=
                    node.v0 + context.surface_depth * node.v1;
            case Kind::Steep:
                return context.steepness > 0.55;
            case Kind::AbovePreliminarySurface:
                return context.y >= context.surface_y;
            case Kind::Hole:
                return false;
            case Kind::Temperature:
                return context.temperature >= node.f0 && context.temperature <= node.f1;
            default:
                return false;
            }
        }

        std::unordered_map<std::string, nlohmann::json> rules_raw_;
        std::unordered_map<std::string, nlohmann::json> conditions_raw_;
        std::unordered_map<std::string, NodePtr> rules_;
        std::unordered_map<std::string, NodePtr> conditions_;
        NodePtr root_;
        BlockResolver block_;
        BiomeResolver biome_;
        const DensityGraph* density_ = nullptr;
        int min_y_ = -64;
    };

} // namespace terila::world::worldgen
