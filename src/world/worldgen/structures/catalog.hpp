#pragma once

// The structures the game assembles from a jigsaw start pool. Every one of them is named in
// `worldgen/structures.json` and placed by one of the sets in `worldgen/structure_sets.json`,
// so the catalog is read from those two files instead of being spelled out in code: a
// structure the data adds appears here without a new line of C++.

#include "data/input_store.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terila::world::worldgen {

    // One `minecraft:jigsaw` entry of structures.json, flattened into the few fields the
    // assembler reads.
    struct JigsawSpec {
        std::string id;
        std::string set_id;
        std::string start_pool;
        std::string start_jigsaw_name;
        int start_height = 0;
        bool has_start_height = false;
        bool project_start_to_heightmap = false;
        int size = 0;
        int max_distance = 0;
        std::string terrain_adaptation;
        bool use_expansion_hack = false;
        std::vector<std::string> biome_names;
    };

    inline bool read_worldgen_doc(const char* path, nlohmann::json& out) {
        std::string text;
        if (!data::input::read_text(path, text)) {
            return false;
        }
        try {
            out = nlohmann::json::parse(text);
        }
        catch (const std::exception&) {
            return false;
        }
        return out.contains("data") && out["data"].is_array();
    }

    // The biome tags the game ships, one entry per tag: a structure names its biomes as a tag
    // far more often than as a list, so the tag has to be expanded before a site is tested.
    inline const std::unordered_map<std::string, std::vector<std::string>>& biome_tag_table() {
        static const std::unordered_map<std::string, std::vector<std::string>> table = [] {
            std::unordered_map<std::string, std::vector<std::string>> loaded;
            std::string text;
            if (!data::input::read_text("tags/all.json", text)) {
                return loaded;
            }
            try {
                const nlohmann::json parsed = nlohmann::json::parse(text);
                for (const nlohmann::json& registry : parsed.at("data")) {
                    if (registry.value("registry", std::string()) != "minecraft:worldgen/biome") {
                        continue;
                    }
                    for (const nlohmann::json& tag : registry.at("tags")) {
                        std::vector<std::string>& names = loaded[tag.value("id", std::string())];
                        for (const nlohmann::json& value : tag.at("values")) {
                            const std::string name = value.is_string()
                                ? value.get<std::string>()
                                : value.value("id", std::string());
                            if (!name.empty()) {
                                names.push_back(name);
                            }
                        }
                    }
                    break;
                }
            }
            catch (const std::exception&) {
            }
            return loaded;
        }();
        return table;
    }

    // A `biomes` field names either one tag (`#minecraft:has_structure/village_plains`) or a
    // list of them, each with or without a biome of its own.
    inline std::vector<std::string> expand_biomes(const nlohmann::json& field) {
        std::vector<std::string> names;
        const auto add = [&names](const std::string& entry) {
            if (entry.empty()) {
                return;
            }
            if (entry.front() != '#') {
                names.push_back(entry);
                return;
            }
            const auto found = biome_tag_table().find(entry.substr(1));
            if (found == biome_tag_table().end()) {
                return;
            }
            for (const std::string& name : found->second) {
                if (name.front() != '#') {
                    names.push_back(name);
                }
            }
        };
        if (field.is_string()) {
            add(field.get<std::string>());
        }
        else if (field.is_array()) {
            for (const nlohmann::json& entry : field) {
                add(entry.is_string() ? entry.get<std::string>()
                                      : entry.value("id", std::string()));
            }
        }
        return names;
    }

    // The structures a set may place, so a set that names five village kinds hands each kind
    // the placement of the one set they share.
    inline std::vector<JigsawSpec> jigsaw_specs() {
        std::vector<JigsawSpec> specs;
        nlohmann::json structures;
        nlohmann::json sets;
        if (!read_worldgen_doc("worldgen/structures.json", structures) ||
            !read_worldgen_doc("worldgen/structure_sets.json", sets)) {
            return specs;
        }

        std::unordered_map<std::string, std::string> set_of;
        for (const nlohmann::json& set : sets.at("data")) {
            const std::string set_id = set.value("id", std::string());
            const nlohmann::json& data = set.at("data");
            if (!data.contains("structures")) {
                continue;
            }
            for (const nlohmann::json& member : data.at("structures")) {
                const std::string id = member.value("structure", std::string());
                if (!id.empty()) {
                    set_of.emplace(id, set_id);
                }
            }
        }

        for (const nlohmann::json& entry : structures.at("data")) {
            const nlohmann::json& data = entry.at("data");
            if (data.value("type", std::string()) != "minecraft:jigsaw") {
                continue;
            }
            JigsawSpec spec;
            spec.id = entry.value("id", std::string());
            const auto set = set_of.find(spec.id);
            spec.set_id = set == set_of.end() ? std::string() : set->second;
            spec.start_pool = data.value("start_pool", std::string());
            spec.start_jigsaw_name = data.value("start_jigsaw_name", std::string());
            spec.size = data.value("size", 0);
            spec.max_distance = data.value("max_distance_from_center", 0);
            spec.terrain_adaptation = data.value("terrain_adaptation", "minecraft:none");
            spec.use_expansion_hack = data.value("use_expansion_hack", false);
            spec.project_start_to_heightmap = data.contains("project_start_to_heightmap");
            if (data.contains("start_height") && data.at("start_height").is_object()) {
                const nlohmann::json& height = data.at("start_height");
                if (height.contains("absolute")) {
                    spec.start_height = height.at("absolute").get<int>();
                    spec.has_start_height = true;
                }
            }
            if (data.contains("biomes")) {
                spec.biome_names = expand_biomes(data.at("biomes"));
            }
            if (spec.start_pool.empty()) {
                continue;
            }
            specs.push_back(std::move(spec));
        }
        return specs;
    }

} // namespace terila::world::worldgen
