#pragma once

// The structures Terila builds today. The game assembles these from code, so the templates
// each one can pick from are named in a table here; how often and how far apart they appear
// comes from the data (worldgen/structure_sets.json), and the height is the surface under
// the site. A chunk asks for the sites that reach into it and writes the part that is its own.

#include "data/input_store.hpp"
#include "world/terrain_biomes.hpp"
#include "world/worldgen/structures/placement.hpp"
#include "world/worldgen/structures/template.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terila::world::worldgen {

    struct StructureKind {
        std::string id;
        std::string set_id;
        std::vector<std::string> templates;
        // A shipwreck lies on the sea floor; a portal sits on dry land.
        bool underwater = false;
    };

    // Ruined portals are the ones the game builds from single templates, so they are the
    // first structure Terila places: no jigsaw assembly is needed to finish one.
    inline std::vector<StructureKind> built_structures() {
        StructureKind portal;
        portal.id = "minecraft:ruined_portal";
        portal.set_id = "minecraft:ruined_portals";
        for (int index = 1; index <= 10; ++index) {
            portal.templates.push_back("minecraft:ruined_portal/portal_" +
                std::to_string(index));
        }
        for (int index = 1; index <= 3; ++index) {
            portal.templates.push_back("minecraft:ruined_portal/giant_portal_" +
                std::to_string(index));
        }
        // A shipwreck ships as whole hulls, so it is placed without assembling a jigsaw.
        StructureKind wreck;
        wreck.id = "minecraft:shipwreck";
        wreck.set_id = "minecraft:shipwrecks";
        wreck.underwater = true;
        for (const char* hull : { "rightsideup_full", "upsidedown_full", "sideways_full" }) {
            wreck.templates.push_back(std::string("minecraft:shipwreck/") + hull);
        }

        StructureKind beached = wreck;
        beached.id = "minecraft:shipwreck_beached";
        beached.underwater = false;

        std::vector<StructureKind> kinds;
        kinds.push_back(std::move(portal));
        kinds.push_back(std::move(wreck));
        kinds.push_back(std::move(beached));
        return kinds;
    }

    class Structures {
    public:
        bool init(std::uint64_t seed, const terrain::IdResolver& block,
            const std::function<int(std::string_view)>& biome_id, std::vector<StructureKind> kinds,
            const StateTextResolver& state = {}) {
            seed_ = seed;
            kinds_.clear();
            const StateTextResolver resolve = state ? state : StateTextResolver([&block](std::string_view text) {
                // The generator knows block names, not states, so a palette entry lands on
                // the block's default state.
                const std::size_t bracket = text.find('[');
                const std::string_view name =
                    bracket == std::string_view::npos ? text : text.substr(0, bracket);
                const auto id = block(name);
                return id ? *id : 0;
            });

            for (StructureKind& kind : kinds) {
                Item item;
                item.spread = spread_of(kind.set_id);
                if (item.spread.spacing <= 0) {
                    continue;
                }
                for (const std::string& id : kind.templates) {
                    StructureTemplate tmpl;
                    if (load_template(id, resolve, tmpl) && !tmpl.empty()) {
                        item.templates.push_back(std::move(tmpl));
                    }
                }
                if (item.templates.empty()) {
                    continue;
                }
                item.id = std::move(kind.id);
                item.underwater = kind.underwater;
                // A structure only goes where its biome tag allows it to.
                item.biomes = biomes_of(item.id, biome_id);
                kinds_.push_back(std::move(item));
            }
            return !kinds_.empty();
        }

        bool empty() const { return kinds_.empty(); }

        // Writes every structure that reaches into the chunk. `min_ground` keeps a structure
        // out of the water: a site whose surface is below it is left alone.
        template<class SurfaceFn, class BiomeFn, class Write>
        int apply(std::int32_t chunk_x, std::int32_t chunk_z, std::int32_t sea_level,
            SurfaceFn&& surface, BiomeFn&& biome_of, Write&& write) const {
            int placed = 0;
            for (const Item& item : kinds_) {
                const std::int32_t base_x = spread_cell(chunk_x, item.spread.spacing);
                const std::int32_t base_z = spread_cell(chunk_z, item.spread.spacing);
                // A template reaches at most a few chunks, so the neighbouring cells are the
                // only ones that can cover this chunk.
                for (std::int32_t gz = base_z - 2; gz <= base_z + 2; ++gz) {
                    for (std::int32_t gx = base_x - 2; gx <= base_x + 2; ++gx) {
                        const StructureSite site = spread_site(seed_, item.spread, gx, gz);
                        if (!site.valid) {
                            continue;
                        }
                        const StructureTemplate& tmpl = item.templates[template_for(item, site)];
                        if (!covers(site, tmpl, chunk_x, chunk_z)) {
                            continue;
                        }
                        const std::int32_t centre_x = site.chunk_x * 16 + 8;
                        const std::int32_t centre_z = site.chunk_z * 16 + 8;
                        const std::int32_t ground = surface(centre_x, centre_z);
                        // A wreck lies on the sea floor and a portal on dry land, so the
                        // waterline decides which sites each kind accepts.
                        if (item.underwater ? (ground >= sea_level) : (ground < sea_level)) {
                            continue;
                        }
                        if (!item.biomes.empty()) {
                            const int biome = biome_of(centre_x, centre_z);
                            if (std::find(item.biomes.begin(), item.biomes.end(), biome) ==
                                item.biomes.end()) {
                                continue;
                            }
                        }
                        const std::int32_t origin_x = centre_x - tmpl.size_x / 2;
                        const std::int32_t origin_z = centre_z - tmpl.size_z / 2;
                        // Two thirds of the template sit above the ground, the rest is the
                        // buried part of the structure.
                        const std::int32_t origin_y = ground - tmpl.size_y * 2 / 3;
                        placed += apply_template(tmpl, origin_x, origin_y, origin_z, chunk_x,
                            chunk_z, write);
                    }
                }
            }
            return placed;
        }

    private:
        struct Item {
            std::string id;
            StructureSpread spread;
            std::vector<StructureTemplate> templates;
            std::vector<int> biomes;
            bool underwater = false;
        };

        // The tags the game ships, loaded once: a structure's biome field is usually a tag.
        static const std::unordered_map<std::string, std::vector<std::string>>& biome_tags() {
            static const std::unordered_map<std::string, std::vector<std::string>> table = [] {
                std::unordered_map<std::string, std::vector<std::string>> loaded;
                std::string text;
                if (!data::input::read_text("tags/all.json", text)) {
                    return loaded;
                }
                try {
                    const nlohmann::json parsed = nlohmann::json::parse(text);
                    // The dump is one entry per registry; the biome tags are the ones a
                    // structure names in its `biomes` field.
                    for (const nlohmann::json& registry : parsed.at("data")) {
                        if (registry.value("registry", std::string()) != "minecraft:worldgen/biome") {
                            continue;
                        }
                        for (const nlohmann::json& tag : registry.at("tags")) {
                            const std::string id = tag.value("id", std::string());
                            for (const nlohmann::json& value : tag.at("values")) {
                                const std::string name = value.is_string()
                                    ? value.get<std::string>()
                                    : value.value("id", std::string());
                                if (!name.empty()) {
                                    loaded[id].push_back(name);
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

        // The biomes a structure may sit in, from its `biomes` field in structures.json.
        static std::vector<int> biomes_of(const std::string& structure_id,
            const std::function<int(std::string_view)>& biome_id) {
            std::vector<int> ids;
            std::string text;
            if (!data::input::read_text("worldgen/structures.json", text)) {
                return ids;
            }
            try {
                const nlohmann::json parsed = nlohmann::json::parse(text);
                for (const nlohmann::json& entry : parsed.at("data")) {
                    if (entry.at("id").get<std::string>() != structure_id) {
                        continue;
                    }
                    const nlohmann::json& biomes = entry.at("data").at("biomes");
                    std::vector<std::string> names;
                    if (biomes.is_string()) {
                        std::string tag = biomes.get<std::string>();
                        if (!tag.empty() && tag.front() == '#') {
                            tag.erase(0, 1);
                        }
                        const auto found = biome_tags().find(tag);
                        if (found != biome_tags().end()) {
                            names = found->second;
                        }
                    }
                    else if (biomes.is_array()) {
                        for (const nlohmann::json& value : biomes) {
                            names.push_back(value.get<std::string>());
                        }
                    }
                    for (const std::string& name : names) {
                        const int id = biome_id(name);
                        if (id >= 0) {
                            ids.push_back(id);
                        }
                    }
                    break;
                }
            }
            catch (const std::exception&) {
            }
            return ids;
        }

        // The placement block of a set: salt, spacing and separation from the data.
        static StructureSpread spread_of(const std::string& set_id) {
            StructureSpread spread;
            std::string text;
            if (!data::input::read_text("worldgen/structure_sets.json", text)) {
                return spread;
            }
            try {
                const nlohmann::json parsed = nlohmann::json::parse(text);
                for (const nlohmann::json& entry : parsed.at("data")) {
                    if (entry.at("id").get<std::string>() != set_id) {
                        continue;
                    }
                    const nlohmann::json& placement = entry.at("data").at("placement");
                    spread.salt = placement.value("salt", 0);
                    spread.spacing = placement.value("spacing", 0);
                    spread.separation = placement.value("separation", 0);
                    break;
                }
            }
            catch (const std::exception&) {
                return StructureSpread{};
            }
            return spread;
        }

        std::size_t template_for(const Item& item, const StructureSite& site) const {
            const std::uint64_t mixed = terrain::detail::hash64(seed_ ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(site.chunk_x)) << 32) ^
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(site.chunk_z)) ^ 0xC2B2AE3Dull);
            return static_cast<std::size_t>(mixed % item.templates.size());
        }

        static bool covers(const StructureSite& site, const StructureTemplate& tmpl,
            std::int32_t chunk_x, std::int32_t chunk_z) {
            if (tmpl.empty()) {
                return false;
            }
            const std::int32_t centre_x = site.chunk_x * 16 + 8;
            const std::int32_t centre_z = site.chunk_z * 16 + 8;
            const std::int32_t low_x = centre_x - tmpl.size_x / 2;
            const std::int32_t low_z = centre_z - tmpl.size_z / 2;
            return (low_x >> 4) <= chunk_x && ((low_x + tmpl.size_x - 1) >> 4) >= chunk_x &&
                (low_z >> 4) <= chunk_z && ((low_z + tmpl.size_z - 1) >> 4) >= chunk_z;
        }

        std::uint64_t seed_ = 0;
        std::vector<Item> kinds_;
    };

} // namespace terila::world::worldgen
