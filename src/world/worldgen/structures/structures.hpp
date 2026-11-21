#pragma once

// The structures Terila builds today. The game assembles these from code, so the templates
// each one can pick from are named in a table here; how often and how far apart they appear
// comes from the data (worldgen/structure_sets.json), and the height is the surface under
// the site. A chunk asks for the sites that reach into it and writes the part that is its own.

#include "data/input_store.hpp"
#include "world/terrain_biomes.hpp"
#include "world/worldgen/structures/jigsaw.hpp"
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
        std::vector<std::string> templates;          // For single-template structures (portal, wreck)
        std::string start_pool;                      // For jigsaw structures (outpost, village, city)
        std::string start_jigsaw_name;               // The jigsaw name at the structure center
        int start_height_absolute = 0;               // Absolute Y for underground structures
        bool has_start_height = false;
        bool project_start_to_heightmap = false;     // Project start to WORLD_SURFACE_WG
        // A shipwreck lies on the sea floor; a portal sits on dry land.
        bool underwater = false;
        bool is_jigsaw = false;                      // True if this uses jigsaw assembly
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

        // Pillager outpost: jigsaw structure starting from base_plates pool.
        StructureKind outpost;
        outpost.id = "minecraft:pillager_outpost";
        outpost.set_id = "minecraft:pillager_outposts";
        outpost.start_pool = "minecraft:pillager_outpost/base_plates";
        outpost.start_jigsaw_name = "minecraft:entrance";
        outpost.project_start_to_heightmap = true;
        outpost.is_jigsaw = true;

        // Village variants: jigsaw structures.
        auto make_village = [](const std::string& id, const std::string& set_id,
            const std::string& start_pool) {
            StructureKind v;
            v.id = id;
            v.set_id = set_id;
            v.start_pool = start_pool;
            v.start_jigsaw_name = "minecraft:village_center";
            v.project_start_to_heightmap = true;
            v.is_jigsaw = true;
            return v;
        };

        StructureKind village_plains = make_village("minecraft:village_plains",
            "minecraft:villages", "minecraft:village/plains/town_centers");
        StructureKind village_desert = make_village("minecraft:village_desert",
            "minecraft:villages", "minecraft:village/desert/town_centers");
        StructureKind village_savanna = make_village("minecraft:village_savanna",
            "minecraft:villages", "minecraft:village/savanna/town_centers");
        StructureKind village_snowy = make_village("minecraft:village_snowy",
            "minecraft:villages", "minecraft:village/snowy/town_centers");
        StructureKind village_taiga = make_village("minecraft:village_taiga",
            "minecraft:villages", "minecraft:village/taiga/town_centers");

        // Ancient city: underground jigsaw structure.
        StructureKind ancient_city;
        ancient_city.id = "minecraft:ancient_city";
        ancient_city.set_id = "minecraft:ancient_cities";
        ancient_city.start_pool = "minecraft:ancient_city/city_center";
        ancient_city.start_jigsaw_name = "minecraft:city_anchor";
        ancient_city.start_height_absolute = -27;
        ancient_city.has_start_height = true;
        ancient_city.is_jigsaw = true;

        std::vector<StructureKind> kinds;
        kinds.push_back(std::move(portal));
        kinds.push_back(std::move(wreck));
        kinds.push_back(std::move(beached));
        kinds.push_back(std::move(outpost));
        kinds.push_back(std::move(village_plains));
        kinds.push_back(std::move(village_desert));
        kinds.push_back(std::move(village_savanna));
        kinds.push_back(std::move(village_snowy));
        kinds.push_back(std::move(village_taiga));
        kinds.push_back(std::move(ancient_city));
        return kinds;
    }

    class Structures {
    public:
        bool init(std::uint64_t seed, const terrain::IdResolver& block,
            const std::function<int(std::string_view)>& biome_id, std::vector<StructureKind> kinds,
            const StateTextResolver& state = {}, const StateRotator& rotate = {}) {
            seed_ = seed;
            kinds_.clear();
            jigsaw_kinds_.clear();
            const StateTextResolver resolve = state ? state : StateTextResolver([block](std::string_view text) {
                // The generator knows block names, not states, so a palette entry lands on
                // the block's default state.
                const std::size_t bracket = text.find('[');
                const std::string_view name =
                    bracket == std::string_view::npos ? text : text.substr(0, bracket);
                const auto id = block(name);
                return id ? *id : 0;
            });

            // Kept for the jigsaw assembly: the palettes of the pieces need the full state
            // table, not the block-default fallback, and a turned piece needs the registry's
            // own rotated variants of those states.
            state_ = resolve;
            rotate_ = rotate;
            // The ground a piece stands on is filled with the same block a beard uses.
            filler_ = resolve("minecraft:dirt");
            for (StructureKind& kind : kinds) {
                if (kind.is_jigsaw) {
                    JigsawItem item;
                    item.spread = spread_of(kind.set_id);
                    if (item.spread.spacing <= 0) {
                        continue;
                    }
                    item.id = kind.id;
                    item.start_pool = kind.start_pool;
                    item.start_jigsaw_name = kind.start_jigsaw_name;
                    item.start_height_absolute = kind.start_height_absolute;
                    item.has_start_height = kind.has_start_height;
                    item.project_start_to_heightmap = kind.project_start_to_heightmap;
                    const JigsawBounds bounds = bounds_of(item.id);
                    item.max_depth = bounds.depth;
                    item.max_distance = bounds.distance;
                    item.underwater = kind.underwater;
                    // A structure only goes where its biome tag allows it to.
                    item.biomes = biomes_of(item.id, biome_id);
                    // Preload all templates this pool can produce for faster runtime.
                    auto pool_opt = load_pool(item.start_pool);
                    if (pool_opt) {
                        item.pool_templates = pool_templates(*pool_opt);
                    }
                    jigsaw_kinds_.push_back(std::move(item));
                } else {
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
            }
            return !kinds_.empty() || !jigsaw_kinds_.empty();
        }

        bool empty() const { return kinds_.empty(); }

        // Writes every structure that reaches into the chunk. `sea_level` keeps a structure
        // out of the water: a site whose surface is below it is left alone.
        template<class SurfaceFn, class BiomeFn, class Write>
        int apply(std::int32_t chunk_x, std::int32_t chunk_z, std::int32_t sea_level,
            SurfaceFn&& surface, BiomeFn&& biome_of, Write&& write) const {
            int placed = 0;
            // Regular (single-template) structures.
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
                        // Vanilla lays a whole template down with a turn of its own, so a
                        // wreck or a portal is not always built the same way round.
                        const std::int32_t rotation = rotation_for(site);
                        if (!covers(site, tmpl, rotation, chunk_x, chunk_z)) {
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
                        const std::int32_t origin_x = centre_x -
                            rotated_size_x(rotation, tmpl.size_x, tmpl.size_z) / 2;
                        const std::int32_t origin_z = centre_z -
                            rotated_size_z(rotation, tmpl.size_x, tmpl.size_z) / 2;
                        // Two thirds of the template sit above the ground, the rest is the
                        // buried part of the structure.
                        const std::int32_t origin_y = ground - tmpl.size_y * 2 / 3;
                        // A whole structure also meets the ground under it: on a slope the
                        // lower side is raised instead of hanging over the drop.
                        fill_beard(tmpl, origin_x, origin_y, origin_z, chunk_x, chunk_z,
                            rotation, surface, filler_, write);
                        placed += apply_template(tmpl, origin_x, origin_y, origin_z, chunk_x,
                            chunk_z, rotation, rotate_, write);
                    }
                }
            }
            // Jigsaw structures (outpost, village, ancient city, etc.).
            for (const JigsawItem& item : jigsaw_kinds_) {
                const std::int32_t base_x = spread_cell(chunk_x, item.spread.spacing);
                const std::int32_t base_z = spread_cell(chunk_z, item.spread.spacing);
                for (std::int32_t gz = base_z - 2; gz <= base_z + 2; ++gz) {
                    for (std::int32_t gx = base_x - 2; gx <= base_x + 2; ++gx) {
                        const StructureSite site = spread_site(seed_, item.spread, gx, gz);
                        if (!site.valid) {
                            continue;
                        }
                        const std::int32_t centre_x = site.chunk_x * 16 + 8;
                        const std::int32_t centre_z = site.chunk_z * 16 + 8;
                        // Determine ground height at the structure center.
                        std::int32_t ground = surface(centre_x, centre_z);
                        if (item.has_start_height) {
                            ground = item.start_height_absolute;
                        } else if (item.project_start_to_heightmap) {
                            // Already have the heightmap value from surface().
                        }
                        // Waterline check.
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
                        // Assemble the jigsaw structure.
                        // The start piece is centered at (centre_x, ground, centre_z).
                        // We need a deterministic seed for this structure instance.
                        const std::uint64_t structure_seed = terrain::detail::hash64(seed_ ^
                            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(site.chunk_x)) << 32) ^
                            static_cast<std::uint64_t>(static_cast<std::uint32_t>(site.chunk_z)) ^
                            0x9E3779B97F4A7C15ull);
                        const std::int32_t origin_x = centre_x;
                        const std::int32_t origin_z = centre_z;
                        const std::int32_t origin_y = ground;
                        auto pieces = assemble_jigsaw(item.start_pool, state_,
                            origin_x, origin_y, origin_z, structure_seed, item.max_pieces,
                            item.max_depth, item.max_distance);
                        // Check if any piece reaches this chunk.
                        bool reaches = false;
                        for (const auto& piece : pieces) {
                            const std::int32_t low_x = piece.origin_x;
                            const std::int32_t high_x = piece.origin_x + piece_size_x(piece) - 1;
                            const std::int32_t low_z = piece.origin_z;
                            const std::int32_t high_z = piece.origin_z + piece_size_z(piece) - 1;
                            if ((low_x >> 4) <= chunk_x && (high_x >> 4) >= chunk_x &&
                                (low_z >> 4) <= chunk_z && (high_z >> 4) >= chunk_z) {
                                reaches = true;
                                break;
                            }
                        }
                        if (!reaches) continue;
                        placed += write_jigsaw_pieces(pieces, chunk_x, chunk_z, surface,
                            filler_, rotate_, write);
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

        // How far a jigsaw structure may grow: the depth of its expansion and the distance
        // from its centre its pieces may reach.
        struct JigsawBounds {
            int depth = 7;
            int distance = 80;
        };

        struct JigsawItem {
            std::string id;
            StructureSpread spread;
            std::string start_pool;
            std::string start_jigsaw_name;
            int start_height_absolute = 0;
            bool has_start_height = false;
            bool project_start_to_heightmap = false;
            int max_pieces = 48;
            int max_depth = 7;
            int max_distance = 80;
            bool underwater = false;
            std::vector<int> biomes;
            std::vector<std::string> pool_templates;  // All template IDs this pool can produce
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

        // The expansion bounds of a jigsaw structure: `size` is the number of layers its
        // pieces may grow to and `max_distance_from_center` the reach from the start.
        static JigsawBounds bounds_of(const std::string& structure_id) {
            JigsawBounds bounds;
            std::string text;
            if (!data::input::read_text("worldgen/structures.json", text)) {
                return bounds;
            }
            try {
                const nlohmann::json parsed = nlohmann::json::parse(text);
                for (const nlohmann::json& entry : parsed.at("data")) {
                    if (entry.at("id").get<std::string>() != structure_id) {
                        continue;
                    }
                    const nlohmann::json& structure = entry.at("data");
                    if (structure.value("type", std::string()) != "minecraft:jigsaw") {
                        break;
                    }
                    bounds.depth = structure.value("size", bounds.depth);
                    bounds.distance = structure.value("max_distance_from_center", bounds.distance);
                    break;
                }
            }
            catch (const std::exception&) {
                return JigsawBounds{};
            }
            return bounds;
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

        // The quarter turn a site's template is laid down with. Without a rotation table the
        // blocks would keep their unturned states, so the template stays as it is drawn.
        std::int32_t rotation_for(const StructureSite& site) const {
            if (!rotate_) {
                return 0;
            }
            const std::uint64_t mixed = terrain::detail::hash64(seed_ ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(site.chunk_x)) << 32) ^
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(site.chunk_z)) ^ 0x51ED270Bull);
            return static_cast<std::int32_t>(mixed % 4);
        }

        static bool covers(const StructureSite& site, const StructureTemplate& tmpl,
            std::int32_t rotation, std::int32_t chunk_x, std::int32_t chunk_z) {
            if (tmpl.empty()) {
                return false;
            }
            const std::int32_t size_x = rotated_size_x(rotation, tmpl.size_x, tmpl.size_z);
            const std::int32_t size_z = rotated_size_z(rotation, tmpl.size_x, tmpl.size_z);
            const std::int32_t centre_x = site.chunk_x * 16 + 8;
            const std::int32_t centre_z = site.chunk_z * 16 + 8;
            const std::int32_t low_x = centre_x - size_x / 2;
            const std::int32_t low_z = centre_z - size_z / 2;
            return (low_x >> 4) <= chunk_x && ((low_x + size_x - 1) >> 4) >= chunk_x &&
                (low_z >> 4) <= chunk_z && ((low_z + size_z - 1) >> 4) >= chunk_z;
        }

        std::uint64_t seed_ = 0;
        std::vector<Item> kinds_;
        std::vector<JigsawItem> jigsaw_kinds_;
        StateTextResolver state_;
        StateRotator rotate_;
        std::int32_t filler_ = 0;
    };

} // namespace terila::world::worldgen
