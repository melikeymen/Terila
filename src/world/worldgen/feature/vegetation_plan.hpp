#pragma once

// What a biome grows, read from the game's own feature data instead of a hand-written table.
// A biome does not carry a "one tree per N columns" number: it names placed features, and a
// placed feature asks for a number of starts per chunk, lets a rarity filter thin them out,
// spreads every start inside the chunk square, drops it on a heightmap and then scatters a
// patch of blocks around it. This header walks that chain and turns it into the densities the
// column planter works with: the same number of plants spread over the chunk's 256 columns
// instead of the same number of patches in it. Trees keep the species mix, the trunk and the
// leaf blocks of the configured feature they come from, so a forest mixes birch into oak the
// way the data says and a taiga stands in spruce.

#include "world/terrain_biomes.hpp"
#include "world/worldgen/feature/state_provider.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    class VegetationPlan {
    public:
        // The block resolver is the registry lookup the rest of the generator already uses; a
        // block this checkout does not know is skipped and the biome keeps its table entry.
        bool load(const terrain::IdResolver& block) {
            block_ = block;
            configured_doc_ = read_json("worldgen/features.json");
            placed_doc_ = read_json("worldgen/placed_features.json");
            biomes_doc_ = read_json("worldgen/biomes.json");
            if (!index(configured_doc_, "features", configured_) ||
                !index(placed_doc_, "placed", placed_) ||
                !index(biomes_doc_, "biomes", biomes_)) {
                return false;
            }
            // A flower patch names a provider of its own instead of a block. Without this
            // registry such a patch is skipped and only the patches that spell their block out
            // still grow, so a missing dump costs the meadows their flowers, not the world.
            provider_doc_ = read_json("registries/dump/minecraft/worldgen/block_state_provider.json");
            if (!index_registry(provider_doc_, providers_by_name_)) {
                LOG_WARN(Chunk, "worldgen: the block state providers are missing, named "
                    "flower patches are skipped");
            }
            loaded_ = true;
            return true;
        }

        bool loaded() const noexcept { return loaded_; }

        // The providers the biomes now name, by the index stored in each biome entry.
        const std::vector<StateProvider>& providers() const noexcept { return providers_; }

        // Rewrites the vegetation of the biome table from the data. A field the data does not
        // describe keeps what the table already says, so a dimension whose preset has no such
        // features still looks the way it did.
        bool apply(std::vector<terrain::Biome>& biomes) {
            if (!loaded_) {
                return false;
            }
            for (std::size_t index = 0; index < biomes.size() && index < terrain::kBiomeCount;
                ++index) {
                const auto found = biomes_.find(terrain::kBiomeNames[index]);
                if (found == biomes_.end()) {
                    continue;
                }
                const Plan plan = derive(*found->second);
                terrain::Biome& biome = biomes[index];

                if (plan.trees_per_chunk > 0.0 && !plan.species.empty()) {
                    biome.species = plan.species;
                    biome.tree_chance = chance_of(plan.trees_per_chunk);
                    biome.log = plan.species.front().log;
                    biome.leaves = plan.species.front().leaves;
                }
                if (plan.cactus_per_chunk > 0.0) {
                    biome.cactus_chance = chance_of(plan.cactus_per_chunk);
                }
                if (plan.cover_per_chunk > 0.0 && plan.cover_provider >= 0) {
                    biome.cover_chance = chance_of(plan.cover_per_chunk);
                    biome.cover = plan.cover;
                    biome.cover_provider = plan.cover_provider;
                }
                if (plan.flower_per_chunk > 0.0 && plan.flower_provider >= 0) {
                    biome.flower_chance = chance_of(plan.flower_per_chunk);
                    biome.flower = plan.flower;
                    biome.flower_provider = plan.flower_provider;
                }
                LOG_DEBUG(Chunk, "worldgen: {} grows {} tree per chunk {} species, cover {}"
                    "per chunk, {} cactus, {} flowers", terrain::kBiomeNames[index],
                    plan.trees_per_chunk, plan.species.size(), plan.cover_per_chunk,
                    plan.cactus_per_chunk, plan.flower_per_chunk);
                if (biome.flower_provider >= 0) {
                    LOG_DEBUG(Chunk, "worldgen: {} flowers [{}]", terrain::kBiomeNames[index],
                        join(providers_[static_cast<std::size_t>(biome.flower_provider)].names()));
                }
                if (biome.cover_provider >= 0) {
                    LOG_DEBUG(Chunk, "worldgen: {} cover [{}]", terrain::kBiomeNames[index],
                        join(providers_[static_cast<std::size_t>(biome.cover_provider)].names()));
                }
            }
            return true;
        }

    private:
        using Json = nlohmann::json;

        struct Plan {
            double trees_per_chunk = 0.0;
            std::vector<terrain::TreeSpecies> species;
            double cactus_per_chunk = 0.0;
            double cover_per_chunk = 0.0;
            std::int32_t cover = 0;
            int cover_provider = -1;
            double flower_per_chunk = 0.0;
            std::int32_t flower = 0;
            int flower_provider = -1;
        };

        // One plant per N columns, the shape the column planter understands.
        static int chance_of(double per_chunk) {
            if (per_chunk <= 0.0) {
                return 0;
            }
            const double chance = std::round(256.0 / per_chunk);
            return std::max(1, static_cast<int>(chance));
        }

        static bool index(const Json& root, const char* name,
            std::unordered_map<std::string, const Json*>& out) {
            if (root.is_null()) {
                LOG_WARN(Chunk, "worldgen: the {} registry is missing", name);
                return false;
            }
            if (!root.contains("data") || !root["data"].is_array()) {
                LOG_WARN(Chunk, "worldgen: the {} registry has no entries", name);
                return false;
            }
            for (const auto& entry : root["data"]) {
                const std::string id = entry.value("id", std::string());
                if (id.empty() || !entry.contains("data")) {
                    continue;
                }
                out[id] = &entry["data"];
            }
            return true;
        }

        Plan derive(const Json& biome) {
            Plan plan;
            if (!biome.contains("features") || !biome["features"].is_array()) {
                return plan;
            }
            for (const auto& step : biome["features"]) {
                if (!step.is_array()) {
                    continue;
                }
                for (const auto& entry : step) {
                    if (!entry.is_string()) {
                        continue;
                    }
                    const auto found = placed_.find(entry.get<std::string>());
                    if (found == placed_.end()) {
                        continue;
                    }
                    add_placed(*found->second, plan);
                }
            }
            return plan;
        }

        // A name the data uses can be a configured feature or a placed one that only wraps
        // it. A selector of a savanna names `acacia_checked`, which is the acacia tree with a
        // survival check in front of it, so the wrapper is followed to what actually grows.
        const Json* resolve(const std::string& id) const {
            std::string name = id;
            for (int hop = 0; hop < 8 && !name.empty(); ++hop) {
                if (const auto found = configured_.find(name); found != configured_.end()) {
                    return found->second;
                }
                const auto placed = placed_.find(name);
                if (placed == placed_.end()) {
                    return nullptr;
                }
                name = placed->second->value("feature", std::string());
            }
            return nullptr;
        }

        void add_placed(const Json& placed, Plan& plan) {
            const Json* found = resolve(placed.value("feature", std::string()));
            if (found == nullptr) {
                return;
            }
            double patch = 1.0;
            const double starts = expected_starts(
                placed.contains("placement") ? placed["placement"] : Json::array(), patch);
            if (starts <= 0.0) {
                return;
            }
            const double blocks = starts * patch;
            const Json& configured = *found;
            const std::string type = configured.value("type", std::string());

            if (type == "minecraft:simple_block") {
                // The patch may name its block directly, list a few with weights, or hand the
                // choice to a provider with a field of its own. All three read the same way.
                if (!configured.contains("to_place")) {
                    return;
                }
                StateProvider provider;
                if (!provider.build(configured["to_place"], block_, providers_by_name_) ||
                    provider.empty()) {
                    return;
                }
                const auto& names = provider.names();
                const bool flower = std::any_of(names.begin(), names.end(), is_flower);
                if (flower) {
                    if (blocks > plan.flower_per_chunk) {
                        plan.flower_per_chunk = blocks;
                        plan.flower = provider.first_state();
                        plan.flower_provider = add_provider(std::move(provider));
                    }
                    return;
                }
                if (std::all_of(names.begin(), names.end(), is_cover) &&
                    blocks > plan.cover_per_chunk) {
                    plan.cover_per_chunk = blocks;
                    plan.cover = provider.first_state();
                    plan.cover_provider = add_provider(std::move(provider));
                }
                return;
            }

            if (type == "minecraft:block_column") {
                if (!configured.contains("layers") || !configured["layers"].is_array() ||
                    configured["layers"].empty()) {
                    return;
                }
                const Json& layer = configured["layers"].front();
                const auto name = block_name(layer.value("provider", Json()));
                if (!name) {
                    return;
                }
                if (bare(*name) == "cactus") {
                    plan.cactus_per_chunk += blocks;
                }
                return;
            }

            if (type == "minecraft:tree") {
                if (add_species(configured, 1.0, plan)) {
                    plan.trees_per_chunk += starts;
                }
                return;
            }

            if (type == "minecraft:random_selector" || type == "minecraft:simple_random_selector" ||
                type == "minecraft:weighted_random_selector") {
                add_selector(type, configured, starts, plan);
            }
        }

        // The tree offers of a selector, weighted by the chance the data gives each of them.
        // Simple and weighted selectors pick uniformly or by their own weight list.
        void add_selector(const std::string& type, const Json& selector, double starts,
            Plan& plan) const {
            struct Offer {
                std::string id;
                double weight = 1.0;
            };
            std::vector<Offer> offers;

            if (type == "minecraft:random_selector") {
                // The game rolls every entry in turn and falls through to the default, so the
                // default keeps the share none of the entries took.
                double left = 1.0;
                if (selector.contains("features") && selector["features"].is_array()) {
                    for (const auto& entry : selector["features"]) {
                        const double chance = entry.value("chance", 0.0);
                        left *= std::max(0.0, 1.0 - chance);
                        if (const auto id = configured_id(entry)) {
                            offers.push_back({ *id, chance });
                        }
                    }
                }
                if (const auto id = configured_id(selector.value("default", Json()))) {
                    offers.push_back({ *id, left });
                }
            }
            else if (selector.contains("features") && selector["features"].is_array()) {
                for (const auto& entry : selector["features"]) {
                    const auto id = configured_id(entry);
                    if (!id) {
                        continue;
                    }
                    const double weight = type == "minecraft:weighted_random_selector"
                        ? entry.value("weight", 1.0) : 1.0;
                    offers.push_back({ *id, weight });
                }
            }

            // Only the offers that resolve to a tree this build can grow count, and they share
            // the starts by their weight: a selector whose list is a feature we cannot grow
            // leaves those starts without a tree, the way the game leaves them empty.
            double total = 0.0;
            for (const auto& offer : offers) {
                const Json* found = resolve(offer.id);
                if (found != nullptr && found->value("type", std::string()) == "minecraft:tree") {
                    total += offer.weight;
                }
            }
            if (total <= 0.0) {
                return;
            }
            for (const auto& offer : offers) {
                const Json* found = resolve(offer.id);
                if (found == nullptr || found->value("type", std::string()) != "minecraft:tree") {
                    continue;
                }
                if (add_species(*found, offer.weight, plan)) {
                    plan.trees_per_chunk += starts * offer.weight / total;
                }
            }
        }

        bool add_species(const Json& tree, double weight, Plan& plan) const {
            // The starts are counted by the caller; this only records the species and how
            // often the selector picks it.
            const auto log_name = block_name(tree.value("trunk_provider", Json()));
            const auto leaf_name = block_name(tree.value("foliage_provider", Json()));
            if (!log_name || !leaf_name) {
                return false;
            }
            const auto log = block_(*log_name);
            const auto leaves = block_(*leaf_name);
            if (!log || !leaves) {
                return false;
            }

            // A trunk placer states the base height and how many extra blocks it may roll on
            // top of it; a placer this build does not know keeps a plain four to six.
            int min_trunk = 4;
            int max_trunk = 6;
            if (tree.contains("trunk_placer") && tree["trunk_placer"].is_object()) {
                const Json& placer = tree["trunk_placer"];
                const int base = placer.value("base_height", 4);
                const int rand_a = placer.value("height_rand_a", 0);
                const int rand_b = placer.value("height_rand_b", 0);
                min_trunk = std::max(1, base);
                max_trunk = std::max(min_trunk, base + rand_a + rand_b);
            }

            terrain::TreeSpecies species;
            species.log = *log;
            species.leaves = *leaves;
            species.min_trunk = min_trunk;
            species.max_trunk = max_trunk;
            species.weight = std::max(1, static_cast<int>(std::lround(weight * 1000.0)));
            for (auto& entry : plan.species) {
                if (entry.log == species.log && entry.leaves == species.leaves &&
                    entry.min_trunk == species.min_trunk &&
                    entry.max_trunk == species.max_trunk) {
                    entry.weight += species.weight;
                    return true;
                }
            }
            plan.species.push_back(species);
            return true;
        }

        // How many starts the chain asks for before a biome filter, and how many blocks each
        // start scatters after it. The modifiers are counted in the order the game applies
        // them: everything before `minecraft:biome` decides the anchor, everything after it
        // belongs to the patch around the anchor.
        static double expected_starts(const Json& placement, double& patch) {
            if (!placement.is_array()) {
                return 0.0;
            }
            double starts = 1.0;
            bool in_patch = false;
            for (const auto& modifier : placement) {
                if (!modifier.is_object()) {
                    continue;
                }
                const std::string type = modifier.value("type", std::string());
                if (type == "minecraft:biome") {
                    in_patch = true;
                    continue;
                }
                if (type == "minecraft:count" || type == "minecraft:count_on_every_layer") {
                    const double count = mean(modifier.value("count", Json()));
                    if (in_patch) {
                        patch *= count;
                    }
                    else {
                        starts *= count;
                    }
                }
                else if (type == "minecraft:offset" && in_patch) {
                    // The patch scatters its plants over a few blocks of height, but only the
                    // cell right above the surface can hold one: the attempts that land in the
                    // rest of the band find rock below or air above and place nothing.
                    const Json& spread = modifier.value("y", Json());
                    if (spread.is_object() && spread.contains("min") && spread.contains("max")) {
                        const double low = spread.value("min", 0.0);
                        const double high = spread.value("max", 0.0);
                        patch /= std::max(1.0, high - low + 1.0);
                    }
                }
                else if (type == "minecraft:rarity_filter" && !in_patch) {
                    starts /= std::max(1, modifier.value("chance", 1));
                }
                else if (type == "minecraft:random_chance" && !in_patch) {
                    starts *= modifier.value("chance", 1.0);
                }
                else if (type == "minecraft:noise_threshold_count" && !in_patch) {
                    const double below = modifier.value("below_noise", 0);
                    const double above = modifier.value("above_noise", 0);
                    starts *= 0.5 * (below + above);
                }
            }
            return starts;
        }

        // The average of an integer provider. A uniform or triangle rolls somewhere between
        // its anchors, a weighted list averages by its weights, and a constant is itself.
        static double mean(const Json& provider) {
            if (provider.is_number()) {
                return provider.get<double>();
            }
            if (!provider.is_object()) {
                return 1.0;
            }
            const std::string type = provider.value("type", std::string());
            const auto number = [&provider](const char* key, double fallback) {
                return provider.contains(key) && provider[key].is_number()
                    ? provider[key].get<double>() : fallback;
            };
            if (type == "minecraft:uniform" || type == "minecraft:biased_to_bottom" ||
                type == "minecraft:trapezoid" || type == "minecraft:clamped" ||
                type == "minecraft:clamped_normal") {
                return 0.5 * (number("min_inclusive", 0.0) + number("max_inclusive", 0.0));
            }
            if (type == "minecraft:weighted_list" && provider.contains("distribution") &&
                provider["distribution"].is_array()) {
                double sum = 0.0;
                double total = 0.0;
                for (const auto& entry : provider["distribution"]) {
                    const double weight = entry.value("weight", 0.0);
                    sum += weight * mean(entry.contains("data") ? entry["data"] : Json());
                    total += weight;
                }
                return total > 0.0 ? sum / total : 0.0;
            }
            return number("data", 1.0);
        }

        // The configured feature a selector entry names. The data spells an entry three ways:
        // a bare id, a one field `feature` object, and a nested `feature` object holding it.
        static std::optional<std::string> configured_id(const Json& entry) {
            if (entry.is_string()) {
                return entry.get<std::string>();
            }
            if (!entry.is_object() || !entry.contains("feature")) {
                return std::nullopt;
            }
            const Json& feature = entry["feature"];
            if (feature.is_string()) {
                return feature.get<std::string>();
            }
            if (feature.is_object() && feature.contains("feature") &&
                feature["feature"].is_string()) {
                return feature["feature"].get<std::string>();
            }
            return std::nullopt;
        }

        // The block a provider places, when it names one directly. A provider list is a
        // registry entry of its own and is not resolved here.
        static std::optional<std::string> block_name(const Json& provider) {
            if (provider.is_string()) {
                return provider.get<std::string>();
            }
            if (provider.is_object() && provider.contains("id") && provider["id"].is_string()) {
                return provider["id"].get<std::string>();
            }
            return std::nullopt;
        }

        static std::string_view bare(std::string_view id) {
            const auto colon = id.find(':');
            return colon == std::string_view::npos ? id : id.substr(colon + 1);
        }

        static bool is_cover(std::string_view name) {
            const std::string_view block = bare(name);
            return block == "short_grass" || block == "grass" || block == "fern" ||
                block == "dead_bush";
        }

        static bool is_flower(std::string_view name) {
            const std::string_view block = bare(name);
            return block == "dandelion" || block == "poppy" || block == "blue_orchid" ||
                block == "allium" || block == "azure_bluet" || block == "red_tulip" ||
                block == "orange_tulip" || block == "white_tulip" || block == "pink_tulip" ||
                block == "oxeye_daisy" || block == "cornflower" ||
                block == "lily_of_the_valley" || block == "wildflower" ||
                block == "wildflowers" || block == "torchflower" ||
                block == "closed_eyeblossom" || block == "pink_petals";
        }

        static std::string join(const std::vector<std::string>& names) {
            std::string out;
            for (const auto& name : names) {
                if (!out.empty()) {
                    out += ' ';
                }
                out += name;
            }
            return out;
        }

        // The patch a provider belongs to is appended once per biome that names it: two biomes
        // rarely share a flower field, and a shared one would still be sampled the same way.
        int add_provider(StateProvider provider) {
            providers_.push_back(std::move(provider));
            return static_cast<int>(providers_.size()) - 1;
        }

        // The block state providers, in the shape the registry dump has: the entries live one
        // level down and carry the same id/data pair as the other registries.
        static bool index_registry(const Json& root,
            std::unordered_map<std::string, const Json*>& out) {
            if (root.is_null() || !root.contains("registries")) {
                return false;
            }
            const Json& registries = root["registries"];
            if (!registries.is_object() || !registries.contains("entries") ||
                !registries["entries"].is_array()) {
                return false;
            }
            for (const auto& entry : registries["entries"]) {
                const std::string id = entry.value("id", std::string());
                if (id.empty() || !entry.contains("data")) {
                    continue;
                }
                out[id] = &entry["data"];
            }
            return true;
        }

        // The tests and the server both run with an input directory somewhere above the
        // working directory, and an embedded build answers without a filesystem at all.
        static nlohmann::json read_json(const std::string& relative) {
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
                std::string text((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());
                if (text.empty()) {
                    continue;
                }
                try {
                    return nlohmann::json::parse(text);
                }
                catch (const std::exception& error) {
                    LOG_WARN(Chunk, "worldgen: cannot parse {}: {}", path.string(),
                        error.what());
                    return nlohmann::json{};
                }
            }
            std::string text;
            if (data::input::read_text(relative, text)) {
                try {
                    return nlohmann::json::parse(text);
                }
                catch (const std::exception& error) {
                    LOG_WARN(Chunk, "worldgen: cannot parse {}: {}", relative, error.what());
                }
            }
            LOG_WARN(Chunk, "worldgen: cannot find {}", relative);
            return nlohmann::json{};
        }

        terrain::IdResolver block_;
        Json configured_doc_;
        Json placed_doc_;
        Json biomes_doc_;
        Json provider_doc_;
        std::unordered_map<std::string, const Json*> configured_;
        std::unordered_map<std::string, const Json*> placed_;
        std::unordered_map<std::string, const Json*> biomes_;
        std::unordered_map<std::string, const Json*> providers_by_name_;
        std::vector<StateProvider> providers_;
        bool loaded_ = false;
    };

} // namespace terila::world::worldgen
