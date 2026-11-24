#pragma once

// The chunk's ore and stone veins, drawn the way the game draws them. A vein is not something
// the terrain knows while it paints rock: the game finishes the chunk first and then walks the
// biome's decoration steps, giving every placed feature a stream of its own that starts from
// the chunk's decoration seed, the feature's position in its step and the step number. This
// header rebuilds that chain for the ore features: the seed chain, the order the game sorts the
// features into, the placement modifiers that pick a spot in the chunk, and the vein body
// itself, so a vein lands on the block it lands on in the game instead of wherever a hash puts
// it.
//
// Only `minecraft:ore` is grown here. Every other feature keeps its own hash-based shortcut in
// the rest of the generator; they still take part in the feature order, because that order is
// what decides a feature's index and therefore its seed.

#include "data/input_store.hpp"
#include "world/terrain_biomes.hpp"
#include "world/worldgen/density/noise.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    // One chunk as the decoration pass sees it: the finished rock, and the biome of a column.
    // A block read outside the chunk answers nothing, which is what the vein's target test
    // needs to know about a cell it cannot write anyway.
    struct DecorationView {
        std::int32_t cx = 0;
        std::int32_t cz = 0;
        std::int32_t min_y = -64;
        std::int32_t world_top = 320;
        int sea_level = 63;
        std::vector<std::int32_t>* blocks = nullptr;
        std::int32_t air = 0;
        std::int32_t water = 0;
        std::int32_t lava = 0;
        std::function<int(std::int32_t, int, std::int32_t)> biome_at;
    };

    class OreDecoration {
    public:
        using Json = nlohmann::json;
        using Overlay = std::unordered_map<std::uint64_t, std::int32_t>;

        // The registered biomes in the order the source hands them out, which is the order the
        // ore features are indexed by.
        void build_biomes() {
            biome_names_ = biome_order();
            for (const auto& name : biome_names_) {
                biome_ids_.emplace(name, static_cast<int>(biome_ids_.size()));
            }
            for (const auto& [name, data] : biomes_) {
                (void)data;
                biome_ids_.emplace(name, static_cast<int>(biome_ids_.size()));
            }
            biome_names_.clear();
            biome_names_.reserve(biome_ids_.size());
            biome_names_.resize(biome_ids_.size());
            for (const auto& [name, id] : biome_ids_) {
                biome_names_[static_cast<std::size_t>(id)] = name;
            }
        }

        bool load(std::uint64_t seed, const terrain::IdResolver& block) {
            seed_ = seed;
            block_ = block;
            configured_doc_ = read_json("worldgen/features.json");
            placed_doc_ = read_json("worldgen/placed_features.json");
            biome_doc_ = read_json("worldgen/biomes.json");
            tag_doc_ = read_json("tags/blocks.json");
            parameter_doc_ = read_json("worldgen/biome_parameters.json");
            if (!index(configured_doc_, "features", configured_) ||
                !index(placed_doc_, "placed", placed_) ||
                !index(biome_doc_, "biomes", biomes_)) {
                return false;
            }
            build_biomes();
            load_tags();
            build_features();
            build_order();
            loaded_ = !features_.empty() && !steps_.empty();
            if (!loaded_) {
                LOG_WARN(Chunk, "worldgen: the ore features cannot be read from the data");
            }
            return loaded_;
        }

        bool loaded() const noexcept { return loaded_; }

        // The id a biome name has in this pass, or -1 for a name the data does not list.
        int biome_id(const std::string& name) const {
            const auto found = biome_ids_.find(name);
            return found == biome_ids_.end() ? -1 : found->second;
        }

        // Runs every ore feature of this chunk into `overlay`, and writes the same blocks into
        // the chunk the view points at. The game places features after the carvers, so the view
        // is the carved chunk and a vein sees the caves around it.
        void decorate(const DecorationView& view, Overlay& overlay) const {
            if (!loaded_ || view.blocks == nullptr) {
                return;
            }
            Pass pass;
            pass.view = &view;
            pass.overlay = &overlay;
            pass.heights.resize(256);
            for (int lz = 0; lz < 16; ++lz) {
                for (int lx = 0; lx < 16; ++lx) {
                    pass.heights[static_cast<std::size_t>(lz) * 16 +
                        static_cast<std::size_t>(lx)] = ocean_floor(view, lx, lz);
                }
            }

            const std::int64_t origin_x = static_cast<std::int64_t>(view.cx) * 16;
            const std::int64_t origin_z = static_cast<std::int64_t>(view.cz) * 16;
            const std::int64_t decoration = decoration_seed(origin_x, origin_z);

            for (std::size_t step = 0; step < steps_.size(); ++step) {
                const std::vector<Entry>& order = steps_[step];
                for (std::size_t position = 0; position < order.size(); ++position) {
                    if (order[position].ore < 0) {
                        continue;
                    }
                    const OreFeature& feature = features_[static_cast<std::size_t>(
                        order[position].ore)];
                    RandomSource random = RandomSource::from_seed(static_cast<std::uint64_t>(
                        decoration + static_cast<std::int64_t>(position) +
                        static_cast<std::int64_t>(10000) * static_cast<std::int64_t>(step)));
                    place_feature(pass, feature, random, static_cast<std::int32_t>(origin_x),
                        static_cast<std::int32_t>(origin_z));
                }
            }
        }

    private:
        // One rule of a target: the game tests the block a vein is about to replace against a
        // small tree of predicates rather than against a single block.
        struct Predicate {
            enum class Kind : std::uint8_t { Never, Tag, Height, All, Any, Not, Block };

            Kind kind = Kind::Never;
            std::unordered_set<std::int32_t> states;
            int min_y = 0;
            int max_y = 0;
            std::vector<Predicate> children;

            bool matches(std::int32_t state, int y) const {
                switch (kind) {
                case Kind::Tag:
                case Kind::Block:
                    return states.find(state) != states.end();
                case Kind::Height:
                    return y >= min_y && y <= max_y;
                case Kind::All:
                    for (const auto& child : children) {
                        if (!child.matches(state, y)) {
                            return false;
                        }
                    }
                    return true;
                case Kind::Any:
                    for (const auto& child : children) {
                        if (child.matches(state, y)) {
                            return true;
                        }
                    }
                    return false;
                case Kind::Not:
                    return children.empty() ? true : !children.front().matches(state, y);
                case Kind::Never:
                    break;
                }
                return false;
            }
        };

        struct Target {
            Predicate where;
            std::int32_t state = 0;
        };

        // One placement modifier. The order the data lists them in is the order the game runs
        // them, and every one of them may draw from the feature's stream.
        struct Modifier {
            enum class Kind : std::uint8_t { Count, Rarity, InSquare, HeightRange, Biome };

            struct Anchor {
                enum class Kind : std::uint8_t { Absolute, AboveBottom, BelowTop, SeaLevel };
                Kind kind = Kind::Absolute;
                int value = 0;
            };

            Kind kind = Kind::Count;
            int chance = 1;
            bool count_uniform = false;
            int count_min = 1;
            int count_max = 1;
            bool trapezoid = false;
            int plateau = 0;
            Anchor min_anchor;
            Anchor max_anchor;
        };

        struct OreFeature {
            std::string name;
            int size = 0;
            float discard = 0.0f;
            std::vector<Target> targets;
            std::vector<Modifier> modifiers;
            std::vector<std::uint8_t> biomes;
        };

        // A placed feature in the game's order: its name for the log, and the ore it grows when
        // this checkout grows one.
        struct Entry {
            std::string name;
            int ore = -1;
        };

        // Everything one chunk needs while the pass runs, carried explicitly so two threads
        // generating two chunks cannot see each other's chunk.
        struct Pass {
            const DecorationView* view = nullptr;
            Overlay* overlay = nullptr;
            std::vector<int> heights;
        };

        static Json read_json(const std::string& relative) {
            std::error_code ec;
            for (const char* prefix : { "input/", "", "../input/", "../../input/" }) {
                const std::filesystem::path path = std::string(prefix) + relative;
                if (!std::filesystem::is_regular_file(path, ec) || ec) {
                    ec.clear();
                    continue;
                }
                std::ifstream file(path, std::ios::binary);
                if (!file) {
                    continue;
                }
                const std::string text((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());
                if (text.empty()) {
                    continue;
                }
                try {
                    return Json::parse(text);
                }
                catch (const std::exception& error) {
                    LOG_WARN(Chunk, "worldgen: cannot parse {}: {}", path.string(),
                        error.what());
                    return Json{};
                }
            }
            std::string text;
            if (data::input::read_text(relative, text) && !text.empty()) {
                try {
                    return Json::parse(text);
                }
                catch (const std::exception& error) {
                    LOG_WARN(Chunk, "worldgen: cannot parse {}: {}", relative, error.what());
                }
            }
            LOG_WARN(Chunk, "worldgen: cannot find {}", relative);
            return Json{};
        }

        static bool index(const Json& root, const char* name,
            std::unordered_map<std::string, const Json*>& out) {
            if (!root.contains("data") || !root["data"].is_array()) {
                LOG_WARN(Chunk, "worldgen: the {} registry has no entries", name);
                return false;
            }
            for (const auto& entry : root["data"]) {
                const std::string id = entry.value("id", std::string());
                if (!id.empty() && entry.contains("data")) {
                    out[id] = &entry["data"];
                }
            }
            return true;
        }

        // The tags a vein tests its host against, resolved to the state ids this checkout
        // stores. A tag value names a block and the resolver hands back its default state,
        // which is what the generator paints for that block.
        void load_tags() {
            if (!tag_doc_.contains("data") || !tag_doc_["data"].is_array()) {
                return;
            }
            for (const auto& entry : tag_doc_["data"]) {
                const std::string id = entry.value("id", std::string());
                if (id.empty() || !entry.contains("values") || !entry["values"].is_array()) {
                    continue;
                }
                std::unordered_set<std::int32_t>& states = tags_[id];
                for (const auto& value : entry["values"]) {
                    if (!value.is_string()) {
                        continue;
                    }
                    if (const auto state = block_(value.get<std::string>())) {
                        states.insert(*state);
                    }
                }
            }
        }

        Predicate parse_predicate(const Json& value) const {
            Predicate predicate;
            if (!value.is_object()) {
                return predicate;
            }
            const std::string type = value.value("predicate_type", std::string());
            if (type == "minecraft:tag_match") {
                predicate.kind = Predicate::Kind::Tag;
                const auto found = tags_.find(value.value("tag", std::string()));
                if (found != tags_.end()) {
                    predicate.states = found->second;
                }
                if (predicate.states.empty()) {
                    predicate.kind = Predicate::Kind::Never;
                }
            }
            else if (type == "minecraft:block_match") {
                predicate.kind = Predicate::Kind::Block;
                if (const auto state = block_(value.value("block", std::string()))) {
                    predicate.states.insert(*state);
                }
                if (predicate.states.empty()) {
                    predicate.kind = Predicate::Kind::Never;
                }
            }
            else if (type == "minecraft:height_match") {
                predicate.kind = Predicate::Kind::Height;
                predicate.min_y = value.value("min_inclusive",
                    std::numeric_limits<int>::min());
                predicate.max_y = value.value("max_inclusive",
                    std::numeric_limits<int>::max());
            }
            else if (type == "minecraft:all_of" || type == "minecraft:any_of") {
                predicate.kind = type == "minecraft:all_of" ? Predicate::Kind::All
                    : Predicate::Kind::Any;
                const Json& rules = value.contains("rules") ? value["rules"]
                    : value.value("predicates", Json::array());
                if (rules.is_array()) {
                    for (const auto& rule : rules) {
                        predicate.children.push_back(parse_predicate(rule));
                    }
                }
            }
            else if (type == "minecraft:not") {
                predicate.kind = Predicate::Kind::Not;
                if (value.contains("rule")) {
                    predicate.children.push_back(parse_predicate(value["rule"]));
                }
                else if (value.contains("predicate")) {
                    predicate.children.push_back(parse_predicate(value["predicate"]));
                }
            }
            return predicate;
        }

        static Modifier::Anchor parse_anchor(const Json& value) {
            Modifier::Anchor anchor;
            if (!value.is_object()) {
                return anchor;
            }
            if (value.contains("absolute")) {
                anchor.kind = Modifier::Anchor::Kind::Absolute;
                anchor.value = value.value("absolute", 0);
            }
            else if (value.contains("above_bottom")) {
                anchor.kind = Modifier::Anchor::Kind::AboveBottom;
                anchor.value = value.value("above_bottom", 0);
            }
            else if (value.contains("below_top")) {
                anchor.kind = Modifier::Anchor::Kind::BelowTop;
                anchor.value = value.value("below_top", 0);
            }
            else if (value.contains("relative_to_sea_level")) {
                anchor.kind = Modifier::Anchor::Kind::SeaLevel;
                anchor.value = value.value("relative_to_sea_level", 0);
            }
            return anchor;
        }

        static int resolve(const Modifier::Anchor& anchor, const DecorationView& view) {
            switch (anchor.kind) {
            case Modifier::Anchor::Kind::Absolute:
                return anchor.value;
            case Modifier::Anchor::Kind::AboveBottom:
                return view.min_y + anchor.value;
            case Modifier::Anchor::Kind::BelowTop:
                return view.world_top - 1 - anchor.value;
            case Modifier::Anchor::Kind::SeaLevel:
                return view.sea_level + anchor.value;
            }
            return anchor.value;
        }

        // Only the modifiers an ore vein of this checkout uses are understood; a feature that
        // asks for anything else is dropped whole rather than placed somewhere the game would
        // not have put it.
        bool parse_modifiers(const Json& placement, OreFeature& feature) const {
            if (!placement.is_array()) {
                return false;
            }
            for (const auto& entry : placement) {
                if (!entry.is_object()) {
                    return false;
                }
                const std::string type = entry.value("type", std::string());
                Modifier modifier;
                if (type == "minecraft:count") {
                    modifier.kind = Modifier::Kind::Count;
                    const Json& count = entry.value("count", Json());
                    if (count.is_number()) {
                        modifier.count_min = count.get<int>();
                        modifier.count_max = modifier.count_min;
                    }
                    else if (count.is_object() &&
                        count.value("type", std::string()) == "minecraft:uniform") {
                        modifier.count_uniform = true;
                        modifier.count_min = count.value("min_inclusive", 0);
                        modifier.count_max = count.value("max_inclusive", 0);
                    }
                    else {
                        return false;
                    }
                }
                else if (type == "minecraft:rarity_filter") {
                    modifier.kind = Modifier::Kind::Rarity;
                    modifier.chance = std::max(1, entry.value("chance", 1));
                }
                else if (type == "minecraft:in_square") {
                    modifier.kind = Modifier::Kind::InSquare;
                }
                else if (type == "minecraft:height_range") {
                    modifier.kind = Modifier::Kind::HeightRange;
                    const Json& height = entry.value("height", Json());
                    if (!height.is_object()) {
                        return false;
                    }
                    const std::string provider = height.value("type", std::string());
                    if (provider == "minecraft:trapezoid") {
                        modifier.trapezoid = true;
                        modifier.plateau = height.value("plateau", 0);
                    }
                    else if (provider != "minecraft:uniform") {
                        return false;
                    }
                    modifier.min_anchor = parse_anchor(height.value("min_inclusive", Json()));
                    modifier.max_anchor = parse_anchor(height.value("max_inclusive", Json()));
                }
                else if (type == "minecraft:biome") {
                    modifier.kind = Modifier::Kind::Biome;
                }
                else {
                    return false;
                }
                feature.modifiers.push_back(modifier);
            }
            return true;
        }

        void build_features() {
            std::unordered_map<std::string, int> index_of;
            for (const auto& [id, data] : placed_) {
                const std::string configured = data->value("feature", std::string());
                const auto found = configured_.find(configured);
                if (found == configured_.end()) {
                    continue;
                }
                const Json& config = *found->second;
                if (config.value("type", std::string()) != "minecraft:ore") {
                    continue;
                }
                OreFeature feature;
                feature.name = id;
                feature.size = config.value("size", 0);
                feature.discard = config.value("discard_chance_on_air_exposure", 0.0f);
                if (feature.size <= 0 || feature.size > 64 || !config.contains("targets") ||
                    !config["targets"].is_array()) {
                    continue;
                }
                for (const auto& target : config["targets"]) {
                    if (!target.is_object()) {
                        continue;
                    }
                    Target parsed;
                    parsed.where = parse_predicate(target.value("target", Json()));
                    const auto state = block_(target.value("state", std::string()));
                    if (!state || parsed.where.kind == Predicate::Kind::Never) {
                        continue;
                    }
                    parsed.state = *state;
                    feature.targets.push_back(std::move(parsed));
                }
                if (feature.targets.empty() ||
                    !parse_modifiers(data->value("placement", Json::array()), feature)) {
                    continue;
                }
                feature.biomes.assign(biome_names_.size(), 0);
                index_of[id] = static_cast<int>(features_.size());
                features_.push_back(std::move(feature));
            }

            // Which biome grows which ore, so the biome modifier answers the way the game's
            // biome generation settings do.
            for (const auto& [biome_id, data] : biomes_) {
                const int biome = this->biome_id(biome_id);
                if (biome < 0 || !data->contains("features") ||
                    !(*data)["features"].is_array()) {
                    continue;
                }
                for (const auto& step : (*data)["features"]) {
                    if (!step.is_array()) {
                        continue;
                    }
                    for (const auto& entry : step) {
                        if (!entry.is_string()) {
                            continue;
                        }
                        const auto found = index_of.find(entry.get<std::string>());
                        if (found != index_of.end()) {
                            OreFeature& feature = features_[static_cast<std::size_t>(
                                found->second)];
                            if (static_cast<std::size_t>(biome) < feature.biomes.size()) {
                                feature.biomes[static_cast<std::size_t>(biome)] = 1;
                            }
                        }
                    }
                }
            }
        }

        // The order the game sorts the placed features of every biome into. A feature's place
        // in its step is part of its seed, so the order has to be the game's: one node per biome
        // list entry at its step, an edge from each feature to the next one of the same biome,
        // then a depth first walk that emits a node after everything it leads to, reversed at
        // the end. The walk visits nodes by (step, first appearance of the feature), which is
        // also the order the ties are broken in.
        struct Node {
            int step = 0;
            int feature = 0;
            std::string name;
            int ore = -1;
        };

        void build_order() {
            const std::vector<std::string> sources = biome_order();
            std::unordered_map<std::string, int> feature_index;
            std::unordered_map<std::string, int> node_index;
            std::vector<Node> nodes;
            std::vector<std::vector<int>> edges;
            std::vector<std::vector<int>> per_biome;

            for (const auto& biome : sources) {
                const auto found = biomes_.find(biome);
                if (found == biomes_.end() || !found->second->contains("features") ||
                    !(*found->second)["features"].is_array()) {
                    continue;
                }
                int step = 0;
                std::vector<int> list;
                for (const auto& bucket : (*found->second)["features"]) {
                    if (bucket.is_array()) {
                        for (const auto& entry : bucket) {
                            if (!entry.is_string()) {
                                continue;
                            }
                            const std::string name = entry.get<std::string>();
                            auto feature = feature_index.find(name);
                            if (feature == feature_index.end()) {
                                feature = feature_index.emplace(name,
                                    static_cast<int>(feature_index.size())).first;
                            }
                            const std::string key = name + "#" + std::to_string(step);
                            auto seen = node_index.find(key);
                            if (seen == node_index.end()) {
                                Node node;
                                node.step = step;
                                node.feature = feature->second;
                                node.name = name;
                                node.ore = find_ore(name);
                                seen = node_index.emplace(key,
                                    static_cast<int>(nodes.size())).first;
                                nodes.push_back(std::move(node));
                            }
                            list.push_back(seen->second);
                        }
                    }
                    ++step;
                }
                if (!list.empty()) {
                    per_biome.push_back(std::move(list));
                }
            }

            edges.assign(nodes.size(), {});
            for (const auto& list : per_biome) {
                for (std::size_t i = 0; i + 1 < list.size(); ++i) {
                    std::vector<int>& out = edges[static_cast<std::size_t>(list[i])];
                    if (std::find(out.begin(), out.end(), list[i + 1]) == out.end()) {
                        out.push_back(list[i + 1]);
                    }
                }
            }

            const auto before = [&nodes](int a, int b) {
                const Node& left = nodes[static_cast<std::size_t>(a)];
                const Node& right = nodes[static_cast<std::size_t>(b)];
                if (left.step != right.step) {
                    return left.step < right.step;
                }
                return left.feature < right.feature;
            };
            std::vector<int> order(nodes.size());
            for (std::size_t i = 0; i < order.size(); ++i) {
                order[i] = static_cast<int>(i);
            }
            std::sort(order.begin(), order.end(), before);
            for (auto& out : edges) {
                std::sort(out.begin(), out.end(), before);
            }

            std::vector<char> visited(nodes.size(), 0);
            std::vector<char> visiting(nodes.size(), 0);
            std::vector<int> post;
            post.reserve(nodes.size());
            for (int start : order) {
                std::vector<std::pair<int, std::size_t>> stack;
                stack.emplace_back(start, 0);
                while (!stack.empty()) {
                    int node = stack.back().first;
                    std::size_t edge = stack.back().second;
                    if (edge == 0) {
                        if (visited[static_cast<std::size_t>(node)] != 0) {
                            stack.pop_back();
                            continue;
                        }
                        if (visiting[static_cast<std::size_t>(node)] != 0) {
                            LOG_WARN(Chunk, "worldgen: the feature order of the data holds a "
                                "cycle at {}", nodes[static_cast<std::size_t>(node)].name);
                            stack.pop_back();
                            continue;
                        }
                        visiting[static_cast<std::size_t>(node)] = 1;
                    }
                    const std::vector<int>& out = edges[static_cast<std::size_t>(node)];
                    if (edge < out.size()) {
                        stack.back().second = edge + 1;
                        const int next = out[edge];
                        if (visited[static_cast<std::size_t>(next)] == 0) {
                            stack.emplace_back(next, 0);
                        }
                        continue;
                    }
                    visiting[static_cast<std::size_t>(node)] = 0;
                    visited[static_cast<std::size_t>(node)] = 1;
                    post.push_back(node);
                    stack.pop_back();
                }
            }
            std::reverse(post.begin(), post.end());

            int max_step = 0;
            for (const auto& node : nodes) {
                max_step = std::max(max_step, node.step);
            }
            steps_.assign(static_cast<std::size_t>(max_step) + 1, {});
            for (int node : post) {
                const Node& entry = nodes[static_cast<std::size_t>(node)];
                steps_[static_cast<std::size_t>(entry.step)].push_back({ entry.name,
                    entry.ore });
            }
        }

        int find_ore(const std::string& name) const {
            for (std::size_t index = 0; index < features_.size(); ++index) {
                if (features_[index].name == name) {
                    return static_cast<int>(index);
                }
            }
            return -1;
        }

        // The order the biome source hands its biomes to the sorter. The parameter list of the
        // source is that order; a checkout without it falls back to the registry order.
        std::vector<std::string> biome_order() const {
            std::vector<std::string> out;
            const Json* overworld = nullptr;
            if (parameter_doc_.contains("data") && parameter_doc_["data"].is_object()) {
                for (const auto& [key, value] : parameter_doc_["data"].items()) {
                    if (key.find("overworld") != std::string::npos) {
                        overworld = &value;
                        break;
                    }
                }
            }
            if (overworld != nullptr && overworld->contains("values") &&
                (*overworld)["values"].contains("biomes") &&
                (*overworld)["values"]["biomes"].is_array()) {
                for (const auto& entry : (*overworld)["values"]["biomes"]) {
                    const std::string name = entry.value("biome", std::string());
                    if (!name.empty()) {
                        out.push_back(name);
                    }
                }
            }
            if (!out.empty()) {
                return out;
            }
            for (const auto& [id, data] : biomes_) {
                (void)data;
                out.push_back(id);
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        // The chunk's decoration seed: two odd longs from a stream seeded with the level seed,
        // mixed with the chunk's origin and seeded again.
        std::int64_t decoration_seed(std::int64_t origin_x, std::int64_t origin_z) const {
            RandomSource random = RandomSource::from_seed(seed_);
            const std::uint64_t x_scale = random.next_long() | 1ull;
            const std::uint64_t z_scale = random.next_long() | 1ull;
            const std::uint64_t mixed = static_cast<std::uint64_t>(origin_x) * x_scale +
                static_cast<std::uint64_t>(origin_z) * z_scale;
            return static_cast<std::int64_t>(mixed ^ seed_);
        }

        // The modifier chain: a number of starts, each moved inside the chunk, dropped on a
        // height and filtered by its biome, then the vein body. Every start draws its square
        // and its height before the body runs, in the order the data lists the modifiers.
        void place_feature(Pass& pass, const OreFeature& feature, RandomSource& random,
            std::int32_t origin_x, std::int32_t origin_z) const {
            int count = 1;
            for (const Modifier& modifier : feature.modifiers) {
                if (modifier.kind == Modifier::Kind::Count) {
                    count = modifier.count_uniform
                        ? modifier.count_min + random.next_int(
                            std::max(1, modifier.count_max - modifier.count_min + 1))
                        : modifier.count_min;
                }
            }
            for (int start = 0; start < count; ++start) {
                std::int32_t x = origin_x;
                std::int32_t z = origin_z;
                std::int32_t y = pass.view->min_y;
                bool drop = false;
                for (const Modifier& modifier : feature.modifiers) {
                    switch (modifier.kind) {
                    case Modifier::Kind::Count:
                        break;
                    case Modifier::Kind::Rarity:
                        if (!(random.next_float() < 1.0f / static_cast<float>(modifier.chance))) {
                            drop = true;
                        }
                        break;
                    case Modifier::Kind::InSquare:
                        x += random.next_int(16);
                        z += random.next_int(16);
                        break;
                    case Modifier::Kind::HeightRange: {
                        const int low = resolve(modifier.min_anchor, *pass.view);
                        const int high = resolve(modifier.max_anchor, *pass.view);
                        if (low > high) {
                            y = low;
                        }
                        else if (modifier.trapezoid) {
                            const int range = high - low;
                            if (modifier.plateau >= range) {
                                y = low + random.next_int(range + 1);
                            }
                            else {
                                const int plateau_start = (range - modifier.plateau) / 2;
                                const int plateau_end = range - plateau_start;
                                y = low + random.next_int(plateau_end + 1) +
                                    random.next_int(plateau_start + 1);
                            }
                        }
                        else {
                            y = low + random.next_int(high - low + 1);
                        }
                        break;
                    }
                    case Modifier::Kind::Biome: {
                        const int biome = pass.view->biome_at
                            ? pass.view->biome_at(x, y, z) : -1;
                        if (biome < 0 ||
                            static_cast<std::size_t>(biome) >= feature.biomes.size() ||
                            feature.biomes[static_cast<std::size_t>(biome)] == 0) {
                            drop = true;
                        }
                        break;
                    }
                    }
                }
                if (drop) {
                    continue;
                }
                vein(pass, feature, random, x, y, z);
            }
        }

        // The vein body: a line of growing spheres from one end to the other, every sphere
        // written into the cells whose centre falls inside the radius it rolled.
        void vein(Pass& pass, const OreFeature& feature, RandomSource& random,
            std::int32_t origin_x, std::int32_t origin_y, std::int32_t origin_z) const {
            const DecorationView& view = *pass.view;
            const float direction = random.next_float() * 3.14159265358979323846f;
            const float spread = static_cast<float>(feature.size) / 8.0f;
            const int reach = static_cast<int>(std::ceil(spread));
            const int max_radius = static_cast<int>(std::ceil(
                (static_cast<float>(feature.size) / 16.0f * 2.0f + 1.0f) / 2.0f));
            const double x0 = static_cast<double>(origin_x) + std::sin(direction) * spread;
            const double x1 = static_cast<double>(origin_x) - std::sin(direction) * spread;
            const double z0 = static_cast<double>(origin_z) + std::cos(direction) * spread;
            const double z1 = static_cast<double>(origin_z) - std::cos(direction) * spread;
            const double y0 = origin_y + random.next_int(3) - 2;
            const double y1 = origin_y + random.next_int(3) - 2;
            const int x_start = origin_x - reach - max_radius;
            const int y_start = origin_y - 2 - max_radius;
            const int z_start = origin_z - reach - max_radius;
            const int size_xz = 2 * (reach + max_radius);
            const int size_y = 2 * (2 + max_radius);

            bool reached = false;
            for (int xprobe = x_start; xprobe <= x_start + size_xz && !reached; ++xprobe) {
                for (int zprobe = z_start; zprobe <= z_start + size_xz && !reached; ++zprobe) {
                    const std::size_t column = static_cast<std::size_t>(zprobe & 15) * 16 +
                        static_cast<std::size_t>(xprobe & 15);
                    if (y_start <= pass.heights[column]) {
                        reached = true;
                    }
                }
            }
            if (!reached) {
                return;
            }

            std::vector<double> data(static_cast<std::size_t>(feature.size) * 4);
            for (int i = 0; i < feature.size; ++i) {
                const float progress = static_cast<float>(i) / static_cast<float>(feature.size);
                const double xx = x0 + static_cast<double>(progress) * (x1 - x0);
                const double yy = y0 + static_cast<double>(progress) * (y1 - y0);
                const double zz = z0 + static_cast<double>(progress) * (z1 - z0);
                const double ss = random.next_double() *
                    static_cast<double>(feature.size) / 16.0;
                const double radius = ((std::sin(static_cast<double>(
                    3.14159265358979323846f * progress)) + 1.0) * ss + 1.0) / 2.0;
                const std::size_t base = static_cast<std::size_t>(i) * 4;
                data[base + 0] = xx;
                data[base + 1] = yy;
                data[base + 2] = zz;
                data[base + 3] = radius;
            }

            for (int i = 0; i < feature.size - 1; ++i) {
                if (data[static_cast<std::size_t>(i) * 4 + 3] <= 0.0) {
                    continue;
                }
                for (int j = i + 1; j < feature.size; ++j) {
                    const std::size_t a = static_cast<std::size_t>(i) * 4;
                    const std::size_t b = static_cast<std::size_t>(j) * 4;
                    if (data[b + 3] <= 0.0) {
                        continue;
                    }
                    const double dx = data[a] - data[b];
                    const double dy = data[a + 1] - data[b + 1];
                    const double dz = data[a + 2] - data[b + 2];
                    const double dr = data[a + 3] - data[b + 3];
                    if (dr * dr > dx * dx + dy * dy + dz * dz) {
                        if (dr > 0.0) {
                            data[b + 3] = -1.0;
                        }
                        else {
                            data[a + 3] = -1.0;
                        }
                    }
                }
            }

            const std::size_t volume = static_cast<std::size_t>(size_xz) *
                static_cast<std::size_t>(size_y) * static_cast<std::size_t>(size_xz);
            std::vector<char> tested(volume, 0);

            for (int i = 0; i < feature.size; ++i) {
                const std::size_t base = static_cast<std::size_t>(i) * 4;
                const double radius = data[base + 3];
                if (radius < 0.0) {
                    continue;
                }
                const double xx = data[base];
                const double yy = data[base + 1];
                const double zz = data[base + 2];
                const int x_min = std::max(static_cast<int>(std::floor(xx - radius)), x_start);
                const int y_min = std::max(static_cast<int>(std::floor(yy - radius)), y_start);
                const int z_min = std::max(static_cast<int>(std::floor(zz - radius)), z_start);
                const int x_max = std::max(static_cast<int>(std::floor(xx + radius)), x_min);
                const int y_max = std::max(static_cast<int>(std::floor(yy + radius)), y_min);
                const int z_max = std::max(static_cast<int>(std::floor(zz + radius)), z_min);
                for (int x = x_min; x <= x_max; ++x) {
                    const double xd = (static_cast<double>(x) + 0.5 - xx) / radius;
                    if (xd * xd >= 1.0) {
                        continue;
                    }
                    for (int y = y_min; y <= y_max; ++y) {
                        const double yd = (static_cast<double>(y) + 0.5 - yy) / radius;
                        if (xd * xd + yd * yd >= 1.0) {
                            continue;
                        }
                        for (int z = z_min; z <= z_max; ++z) {
                            const double zd = (static_cast<double>(z) + 0.5 - zz) / radius;
                            if (xd * xd + yd * yd + zd * zd >= 1.0) {
                                continue;
                            }
                            if (y < view.min_y || y >= view.world_top) {
                                continue;
                            }
                            const std::size_t index = static_cast<std::size_t>(x - x_start) +
                                static_cast<std::size_t>(y - y_start) *
                                    static_cast<std::size_t>(size_xz) +
                                static_cast<std::size_t>(z - z_start) *
                                    static_cast<std::size_t>(size_xz) *
                                    static_cast<std::size_t>(size_y);
                            if (tested[index] != 0) {
                                continue;
                            }
                            tested[index] = 1;
                            apply(pass, feature, random, x, y, z);
                        }
                    }
                }
            }
        }

        // One cell inside the vein's box: the first target whose rule accepts the block under
        // it wins, and a target that rolls against air exposure may still drop the cell.
        void apply(Pass& pass, const OreFeature& feature, RandomSource& random, int x, int y,
            int z) const {
            const bool inside = x >= pass.view->cx * 16 && x < pass.view->cx * 16 + 16 &&
                z >= pass.view->cz * 16 && z < pass.view->cz * 16 + 16;
            if (!inside) {
                return;
            }
            const std::int32_t state = current(pass, x, y, z);
            for (const Target& target : feature.targets) {
                if (!target.where.matches(state, y)) {
                    continue;
                }
                bool skip_air_check = true;
                if (feature.discard > 0.0f) {
                    if (feature.discard >= 1.0f) {
                        skip_air_check = false;
                    }
                    else {
                        skip_air_check = random.next_float() >= feature.discard;
                    }
                }
                if (!skip_air_check && touches_air(pass, x, y, z)) {
                    continue;
                }
                write(pass, x, y, z, target.state);
                return;
            }
        }

        static std::int32_t current(const Pass& pass, int x, int y, int z) {
            if (x < pass.view->cx * 16 || x >= pass.view->cx * 16 + 16 ||
                z < pass.view->cz * 16 || z >= pass.view->cz * 16 + 16 || y < pass.view->min_y ||
                y >= pass.view->world_top) {
                return -1;
            }
            const auto found = pass.overlay->find(pack(x, y, z));
            if (found != pass.overlay->end()) {
                return found->second;
            }
            return (*pass.view->blocks)[static_cast<std::size_t>(y - pass.view->min_y) * 256 +
                static_cast<std::size_t>(z & 15) * 16 + static_cast<std::size_t>(x & 15)];
        }

        static bool touches_air(const Pass& pass, int x, int y, int z) {
            const int air = pass.view->air;
            return current(pass, x + 1, y, z) == air || current(pass, x - 1, y, z) == air ||
                current(pass, x, y + 1, z) == air || current(pass, x, y - 1, z) == air ||
                current(pass, x, y, z + 1) == air || current(pass, x, y, z - 1) == air;
        }

        static void write(const Pass& pass, int x, int y, int z, std::int32_t state) {
            (*pass.view->blocks)[static_cast<std::size_t>(y - pass.view->min_y) * 256 +
                static_cast<std::size_t>(z & 15) * 16 + static_cast<std::size_t>(x & 15)] = state;
            (*pass.overlay)[pack(x, y, z)] = state;
        }

        static std::uint64_t pack(int x, int y, int z) {
            return (static_cast<std::uint64_t>(x & 15) << 24) |
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y + 1024) & 0x3FF) << 12) |
                static_cast<std::uint64_t>(z & 15);
        }

        // The first block of a column that is neither air nor a fluid: the height the game
        // reads when it checks whether a vein starts above the ground.
        static int ocean_floor(const DecorationView& view, int lx, int lz) {
            for (int y = view.world_top - 1; y >= view.min_y; --y) {
                const std::int32_t state = (*view.blocks)[
                    static_cast<std::size_t>(y - view.min_y) * 256 +
                    static_cast<std::size_t>(lz) * 16 + static_cast<std::size_t>(lx)];
                if (state != view.air && state != view.water && state != view.lava) {
                    return y;
                }
            }
            return view.min_y - 1;
        }

        terrain::IdResolver block_;
        std::uint64_t seed_ = 0;
        Json configured_doc_;
        Json placed_doc_;
        Json biome_doc_;
        Json tag_doc_;
        Json parameter_doc_;
        std::unordered_map<std::string, const Json*> configured_;
        std::unordered_map<std::string, const Json*> placed_;
        std::unordered_map<std::string, const Json*> biomes_;
        std::unordered_map<std::string, std::unordered_set<std::int32_t>> tags_;
        std::vector<std::string> biome_names_;
        std::unordered_map<std::string, int> biome_ids_;
        std::vector<OreFeature> features_;
        std::vector<std::vector<Entry>> steps_;
        bool loaded_ = false;
    };

} // namespace terila::world::worldgen
