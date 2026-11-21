#pragma once

// Jigsaw structure assembly: a structure starts from a template pool, each piece
// exposes jigsaw blocks that name a target pool and a target jigsaw name. The
// game picks a piece from that pool whose jigsaw block has the matching name,
// aligns the two pieces so the jigsaw blocks face each other, and replaces the
// jigsaw blocks with their final_state (usually air).

#include "data/input_store.hpp"
#include "world/worldgen/structures/template.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace terila::world::worldgen {

    // A jigsaw block entity from a template's NBT.
    struct JigsawBlock {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        std::string name;           // This jigsaw's name (e.g., "minecraft:plate_entry")
        std::string target_pool;    // Pool to draw the next piece from
        std::string target_name;    // Name the next piece's jigsaw must have
        std::string final_state;    // What to replace this jigsaw with after connecting
        std::string joint;          // "aligned" or "rollable" (we only handle aligned)
        std::string orientation;    // From state properties: "north_up", "south_up", etc.
    };

    // Orientation of a jigsaw block, parsed from state properties.
    enum class JigsawOrientation : std::uint8_t {
        NorthUp,
        SouthUp,
        EastUp,
        WestUp,
        UpNorth,
        UpSouth,
        UpEast,
        UpWest,
        DownNorth,
        DownSouth,
        DownEast,
        DownWest,
        Unknown
    };

    inline JigsawOrientation parse_orientation(std::string_view state) {
        const std::size_t bracket = state.find('[');
        if (bracket == std::string_view::npos) {
            return JigsawOrientation::Unknown;
        }
        const std::size_t orient_pos = state.find("orientation=");
        if (orient_pos == std::string_view::npos) {
            return JigsawOrientation::Unknown;
        }
        const std::size_t start = orient_pos + 10;
        const std::size_t end = state.find_first_of(",]", start);
        const std::string_view orient = state.substr(start, end - start);

        if (orient == "north_up") return JigsawOrientation::NorthUp;
        if (orient == "south_up") return JigsawOrientation::SouthUp;
        if (orient == "east_up") return JigsawOrientation::EastUp;
        if (orient == "west_up") return JigsawOrientation::WestUp;
        if (orient == "up_north") return JigsawOrientation::UpNorth;
        if (orient == "up_south") return JigsawOrientation::UpSouth;
        if (orient == "up_east") return JigsawOrientation::UpEast;
        if (orient == "up_west") return JigsawOrientation::UpWest;
        if (orient == "down_north") return JigsawOrientation::DownNorth;
        if (orient == "down_south") return JigsawOrientation::DownSouth;
        if (orient == "down_east") return JigsawOrientation::DownEast;
        if (orient == "down_west") return JigsawOrientation::DownWest;
        return JigsawOrientation::Unknown;
    }

    // Opposite orientation for jigsaw alignment (aligned joint).
    inline JigsawOrientation opposite(JigsawOrientation o) {
        switch (o) {
            case JigsawOrientation::NorthUp: return JigsawOrientation::SouthUp;
            case JigsawOrientation::SouthUp: return JigsawOrientation::NorthUp;
            case JigsawOrientation::EastUp:  return JigsawOrientation::WestUp;
            case JigsawOrientation::WestUp:  return JigsawOrientation::EastUp;
            case JigsawOrientation::UpNorth: return JigsawOrientation::DownSouth;
            case JigsawOrientation::UpSouth: return JigsawOrientation::DownNorth;
            case JigsawOrientation::UpEast:  return JigsawOrientation::DownWest;
            case JigsawOrientation::UpWest:  return JigsawOrientation::DownEast;
            case JigsawOrientation::DownNorth: return JigsawOrientation::UpSouth;
            case JigsawOrientation::DownSouth: return JigsawOrientation::UpNorth;
            case JigsawOrientation::DownEast:  return JigsawOrientation::UpWest;
            case JigsawOrientation::DownWest:  return JigsawOrientation::UpEast;
            default: return JigsawOrientation::Unknown;
        }
    }

    // A piece placed in the world during jigsaw assembly.
    struct PlacedPiece {
        StructureTemplate tmpl;
        std::int32_t origin_x = 0;
        std::int32_t origin_y = 0;
        std::int32_t origin_z = 0;
        std::vector<JigsawBlock> jigsaws;  // Jigsaws in world coordinates
    };

    // A pending jigsaw connection to resolve.
    struct JigsawConnection {
        std::int32_t x, y, z;        // World position of the jigsaw
        JigsawOrientation orientation;
        std::string target_pool;
        std::string target_name;
        std::string final_state;
        std::string joint;
    };

    // A template pool element.
    struct PoolElement {
        enum class Type {
            Single,
            LegacySingle,
            List,
            Feature,
            Empty
        } type = Type::Empty;

        std::string location;           // Template location for single/legacy_single
        std::vector<PoolElement> elements;  // For list_pool_element
        std::string feature;            // For feature_pool_element
        std::string processors;         // Processor list id (or empty)
        std::string projection;         // "rigid" or "terrain_matching"
        int weight = 1;
    };

    // A template pool (from template_pools.json).
    struct TemplatePool {
        std::string id;
        std::vector<PoolElement> elements;
        std::string fallback;
    };

    // Parse a pool element from JSON.
    inline PoolElement parse_pool_element(const nlohmann::json& je) {
        PoolElement pe;
        const std::string type = je.value("element_type", "");
        if (type == "minecraft:single_pool_element") {
            pe.type = PoolElement::Type::Single;
        } else if (type == "minecraft:legacy_single_pool_element") {
            pe.type = PoolElement::Type::LegacySingle;
        } else if (type == "minecraft:list_pool_element") {
            pe.type = PoolElement::Type::List;
        } else if (type == "minecraft:feature_pool_element") {
            pe.type = PoolElement::Type::Feature;
        } else if (type == "minecraft:empty_pool_element") {
            pe.type = PoolElement::Type::Empty;
        }
        pe.location = je.value("location", "");
        pe.feature = je.value("feature", "");
        // processors can be a string (processor list id) or an object {processors: [...]}
        if (je.contains("processors")) {
            const auto& proc = je.at("processors");
            if (proc.is_string()) {
                pe.processors = proc.get<std::string>();
            } else if (proc.is_object() && proc.contains("processors")) {
                // It's an inline processor list, we don't need the details for placement
                pe.processors = "inline";
            }
        }
        pe.projection = je.value("projection", "rigid");
        pe.weight = je.value("weight", 1);
        if (pe.type == PoolElement::Type::List) {
            for (const auto& child : je.value("elements", nlohmann::json::array())) {
                pe.elements.push_back(parse_pool_element(child));
            }
        }
        return pe;
    }

    // Load a template pool from JSON file. The parsed pools are cached because the
    // file is large and we may look up the same pool many times during assembly.
    inline std::optional<TemplatePool> load_pool(const std::string& pool_id) {
        static std::unordered_map<std::string, TemplatePool> cache;
        static bool loaded = false;
        if (!loaded) {
            std::string text;
            if (!data::input::read_text("worldgen/template_pools.json", text)) {
                return std::nullopt;
            }
            try {
                const nlohmann::json parsed = nlohmann::json::parse(text);
                for (const auto& entry : parsed.at("data")) {
                    const std::string id = entry.at("id").get<std::string>();
                    TemplatePool pool;
                    pool.id = id;
                    pool.fallback = entry.at("data").value("fallback", "minecraft:empty");
                    for (const auto& elem : entry.at("data").at("elements")) {
                        pool.elements.push_back(parse_pool_element(elem.at("element")));
                    }
                    cache.emplace(std::move(id), std::move(pool));
                }
            } catch (const std::exception&) {
            }
            loaded = true;
        }
        const auto found = cache.find(pool_id);
        if (found != cache.end()) {
            return found->second;
        }
        return std::nullopt;
    }

    // Extract jigsaw blocks from a template's block_entities.
    inline std::vector<JigsawBlock> extract_jigsaws(const StructureTemplate& tmpl,
        const nlohmann::json& parsed) {
        std::vector<JigsawBlock> jigsaws;
        const auto block_entities = parsed.find("block_entities");
        if (block_entities == parsed.end() || !block_entities->is_array()) {
            return jigsaws;
        }
        for (const auto& be : *block_entities) {
            if (!be.is_object()) continue;
            const auto nbt = be.find("nbt");
            if (nbt == be.end() || !nbt->is_object()) continue;
            if (nbt->value("id", "") != "minecraft:jigsaw") continue;

            JigsawBlock jb;
            const auto pos = be.find("pos");
            if (pos != be.end() && pos->is_array() && pos->size() >= 3) {
                jb.x = pos->at(0).get<std::int32_t>();
                jb.y = pos->at(1).get<std::int32_t>();
                jb.z = pos->at(2).get<std::int32_t>();
            }
            const std::string state = be.value("state", "");
            jb.orientation = state;
            jb.name = nbt->value("name", "");
            jb.target_pool = nbt->value("pool", "");
            jb.target_name = nbt->value("target", "");
            jb.final_state = nbt->value("final_state", "minecraft:air");
            jb.joint = nbt->value("joint", "aligned");
            jigsaws.push_back(jb);
        }
        return jigsaws;
    }

    // Load a template and its jigsaw blocks. Templates are cached because the same
    // template may be loaded many times during jigsaw assembly.
    inline bool load_template_with_jigsaws(std::string_view id,
        const StateTextResolver& resolve,
        StructureTemplate& out_tmpl,
        std::vector<JigsawBlock>& out_jigsaws) {
        static std::unordered_map<std::string, std::pair<StructureTemplate, std::vector<JigsawBlock>>> cache;
        static std::unordered_map<std::string, bool> negative_cache;

        // The mounted input root is part of the key: a test that points the store at a
        // different directory must not read the templates of another one.
        const std::string key = data::input::root() + '|' + std::string(id);
        const auto cached = cache.find(key);
        if (cached != cache.end()) {
            out_tmpl = cached->second.first;
            out_jigsaws = cached->second.second;
            return true;
        }
        if (negative_cache.find(key) != negative_cache.end()) {
            return false;
        }

        std::string text;
        if (!data::input::read_text(template_path(id), text)) {
            // Not negative-cached: the store may simply not be mounted yet, and a template
            // that is missing now can appear once the world's input is in place.
            return false;
        }

        try {
            const nlohmann::json parsed = nlohmann::json::parse(text);
            StructureTemplate tmpl;
            const nlohmann::json& size = parsed.at("size");
            tmpl.size_x = size.at("x").get<std::int32_t>();
            tmpl.size_y = size.at("y").get<std::int32_t>();
            tmpl.size_z = size.at("z").get<std::int32_t>();

            const auto blocks = parsed.find("blocks");
            if (blocks != parsed.end() && blocks->is_object()) {
                for (auto entry = blocks->begin(); entry != blocks->end(); ++entry) {
                    const std::int32_t state = resolve(entry.key());
                    for (const nlohmann::json& position : entry.value()) {
                        if (!position.is_array() || position.size() < 3) continue;
                        TemplateCell cell;
                        cell.x = position.at(0).get<std::int32_t>();
                        cell.y = position.at(1).get<std::int32_t>();
                        cell.z = position.at(2).get<std::int32_t>();
                        cell.state = state;
                        tmpl.cells.push_back(cell);
                    }
                }
            }

            std::vector<JigsawBlock> jigsaws = extract_jigsaws(tmpl, parsed);
            out_tmpl = std::move(tmpl);
            out_jigsaws = std::move(jigsaws);
            cache.emplace(key, std::make_pair(out_tmpl, out_jigsaws));
            return true;
        } catch (const std::exception&) {
            negative_cache.emplace(key, true);
            return false;
        }
    }

    // Pick a random pool element weighted by weight.
    inline const PoolElement* pick_element(const TemplatePool& pool,
        std::uint64_t& rng_state) {
        if (pool.elements.empty()) return nullptr;
        std::uint64_t total = 0;
        for (const auto& e : pool.elements) {
            total += static_cast<std::uint64_t>(std::max(1, e.weight));
        }
        if (total == 0) return nullptr;
        std::uint64_t pick = rng_state % total;
        rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
        for (const auto& e : pool.elements) {
            const std::uint64_t w = static_cast<std::uint64_t>(std::max(1, e.weight));
            if (pick < w) {
                return &e;
            }
            pick -= w;
        }
        return &pool.elements.back();
    }

    // Recursively collect all single template locations from a pool element.
    inline void collect_templates(const PoolElement& elem,
        std::vector<std::string>& out) {
        switch (elem.type) {
            case PoolElement::Type::Single:
            case PoolElement::Type::LegacySingle:
                if (!elem.location.empty()) out.push_back(elem.location);
                break;
            case PoolElement::Type::List:
                for (const auto& child : elem.elements) {
                    collect_templates(child, out);
                }
                break;
            case PoolElement::Type::Feature:
            case PoolElement::Type::Empty:
                break;
        }
    }

    // Get all template locations a pool can produce (for preloading).
    inline std::vector<std::string> pool_templates(const TemplatePool& pool) {
        std::vector<std::string> out;
        for (const auto& elem : pool.elements) {
            collect_templates(elem, out);
        }
        return out;
    }

    // Recursively pick and place pieces from a pool element.
    inline bool place_pool_element(const PoolElement& elem,
        const StateTextResolver& resolve,
        std::vector<PlacedPiece>& placed,
        std::vector<JigsawConnection>& pending,
        std::int32_t origin_x, std::int32_t origin_y, std::int32_t origin_z,
        std::uint64_t& rng) {
        switch (elem.type) {
            case PoolElement::Type::Single:
            case PoolElement::Type::LegacySingle: {
                if (elem.location.empty()) return true;
                StructureTemplate tmpl;
                std::vector<JigsawBlock> jigsaws;
                if (!load_template_with_jigsaws(elem.location, resolve, tmpl, jigsaws)) {
                    return false;
                }
                if (tmpl.empty()) return true;

                PlacedPiece piece;
                piece.tmpl = std::move(tmpl);
                piece.origin_x = origin_x;
                piece.origin_y = origin_y;
                piece.origin_z = origin_z;
                // Convert local jigsaw positions to world coordinates.
                for (auto& jb : jigsaws) {
                    jb.x += origin_x;
                    jb.y += origin_y;
                    jb.z += origin_z;
                }
                piece.jigsaws = std::move(jigsaws);
                placed.push_back(std::move(piece));
                return true;
            }
            case PoolElement::Type::List: {
                for (const auto& child : elem.elements) {
                    if (!place_pool_element(child, resolve, placed, pending,
                            origin_x, origin_y, origin_z, rng)) {
                        return false;
                    }
                }
                return true;
            }
            case PoolElement::Type::Feature:
            case PoolElement::Type::Empty:
                return true;
        }
        return true;
    }

    // One way to continue a jigsaw: an element of the target pool and the jigsaw block
    // inside it that carries the name being looked for.
    struct JigsawCandidate {
        PoolElement elem;
        JigsawBlock jigsaw;
    };

    // Every candidate for (pool, jigsaw name), built once per pair. The pool scan reads
    // whole templates, so caching it keeps a large structure from re-reading the village
    // houses for every street it grows.
    inline const std::vector<JigsawCandidate>& candidates_for(const std::string& pool_id,
        const std::string& target_name, const StateTextResolver& resolve) {
        static std::unordered_map<std::string, std::vector<JigsawCandidate>> cache;
        const std::string key = pool_id + '\x1f' + target_name;
        const auto cached = cache.find(key);
        if (cached != cache.end()) {
            return cached->second;
        }

        std::vector<JigsawCandidate> list;
        auto pool_opt = load_pool(pool_id);
        if (pool_opt) {
            for (const auto& elem : pool_opt->elements) {
                std::vector<std::string> templates;
                collect_templates(elem, templates);
                for (const auto& tmpl_id : templates) {
                    StructureTemplate tmpl;
                    std::vector<JigsawBlock> jigsaws;
                    if (!load_template_with_jigsaws(tmpl_id, resolve, tmpl, jigsaws)) continue;
                    for (const auto& jb : jigsaws) {
                        if (jb.name == target_name) {
                            list.push_back({ elem, jb });
                        }
                    }
                }
            }
        }
        return cache.emplace(key, std::move(list)).first->second;
    }

    // Try to connect a pending jigsaw by picking a piece from its target pool.
    inline bool try_connect_jigsaw(const JigsawConnection& conn,
        const StateTextResolver& resolve,
        std::vector<PlacedPiece>& placed,
        std::vector<JigsawConnection>& new_pending,
        std::uint64_t& rng,
        int depth) {
        if (depth > 50) return false;  // Prevent infinite recursion
        if (conn.joint != "aligned") return false;  // Only handle aligned joints
        if (conn.target_pool.empty()) return true;

        const std::vector<JigsawCandidate>& candidates =
            candidates_for(conn.target_pool, conn.target_name, resolve);
        if (candidates.empty()) {
            // No matching piece; fall back to empty (which does nothing).
            return true;
        }

        // Pick a random candidate.
        const std::uint64_t pick = rng % candidates.size();
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        const JigsawCandidate& chosen = candidates[pick];
        const PoolElement& chosen_elem = chosen.elem;
        const JigsawBlock& matching_jigsaw = chosen.jigsaw;

        // Compute the origin for the new piece so its jigsaw aligns with ours.
        // Our jigsaw is at (conn.x, conn.y, conn.z) with orientation conn.orientation.
        // The matching jigsaw in the new piece is at (matching_jigsaw.x, matching_jigsaw.y, matching_jigsaw.z)
        // with orientation matching_jigsaw.orientation.
        // We need: new_piece_origin + matching_jigsaw.pos = conn.pos
        // and the orientations must be opposite.
        const JigsawOrientation needed_orient = opposite(conn.orientation);
        const JigsawOrientation piece_orient = parse_orientation(matching_jigsaw.orientation);
        if (piece_orient != needed_orient) {
            // Orientation doesn't match; this candidate won't align properly.
            // In vanilla, the piece would be rotated, but we only support aligned joints
            // with matching orientations for now.
            return false;
        }

        const std::int32_t new_origin_x = conn.x - matching_jigsaw.x;
        const std::int32_t new_origin_y = conn.y - matching_jigsaw.y;
        const std::int32_t new_origin_z = conn.z - matching_jigsaw.z;

        // Place the chosen element at the computed origin.
        const std::size_t placed_before = placed.size();
        if (!place_pool_element(chosen_elem, resolve, placed, new_pending,
                new_origin_x, new_origin_y, new_origin_z, rng)) {
            return false;
        }
        if (placed.size() == placed_before) {
            return false;  // Nothing landed, so there is nothing to expand from.
        }

        // The new piece's jigsaws (except the one we just connected) become new pending connections.
        const PlacedPiece& new_piece = placed.back();
        for (const auto& jb : new_piece.jigsaws) {
            if (jb.x == conn.x && jb.y == conn.y && jb.z == conn.z) continue;  // The one we connected
            if (jb.target_pool.empty()) continue;
            JigsawConnection new_conn;
            new_conn.x = jb.x;
            new_conn.y = jb.y;
            new_conn.z = jb.z;
            new_conn.orientation = parse_orientation(jb.orientation);
            new_conn.target_pool = jb.target_pool;
            new_conn.target_name = jb.target_name;
            new_conn.final_state = jb.final_state;
            new_conn.joint = jb.joint;
            new_pending.push_back(new_conn);
        }

        return true;
    }

    // Assemble a jigsaw structure starting from a start pool.
    // Returns the placed pieces. The start piece is placed at the given origin.
    // `max_pieces` bounds the village/street expansion, which in vanilla is bounded by
    // the structure's `size` field: an unbounded breadth-first walk would grow until the
    // memory runs out.
    inline std::vector<PlacedPiece> assemble_jigsaw(const std::string& start_pool_id,
        const StateTextResolver& resolve,
        std::int32_t origin_x, std::int32_t origin_y, std::int32_t origin_z,
        std::uint64_t seed, int max_pieces = 64) {
        std::vector<PlacedPiece> placed;
        std::vector<JigsawConnection> pending;

        auto start_pool_opt = load_pool(start_pool_id);
        if (!start_pool_opt) return placed;

        // Pick and place the start piece.
        const PoolElement* start_elem = pick_element(*start_pool_opt, seed);
        if (!start_elem) return placed;

        if (!place_pool_element(*start_elem, resolve, placed, pending,
                origin_x, origin_y, origin_z, seed)) {
            return placed;
        }

        // Convert the start piece's jigsaws to pending connections.
        for (const auto& piece : placed) {
            for (const auto& jb : piece.jigsaws) {
                if (jb.target_pool.empty()) continue;
                JigsawConnection conn;
                conn.x = jb.x;
                conn.y = jb.y;
                conn.z = jb.z;
                conn.orientation = parse_orientation(jb.orientation);
                conn.target_pool = jb.target_pool;
                conn.target_name = jb.target_name;
                conn.final_state = jb.final_state;
                conn.joint = jb.joint;
                pending.push_back(conn);
            }
        }

        // Resolve pending connections (breadth-first, bounded by the piece cap).
        for (int iteration = 0; iteration < 200 && !pending.empty() &&
            static_cast<int>(placed.size()) < max_pieces; ++iteration) {
            std::vector<JigsawConnection> current = std::move(pending);
            pending.clear();
            for (const auto& conn : current) {
                if (static_cast<int>(placed.size()) >= max_pieces) break;
                try_connect_jigsaw(conn, resolve, placed, pending, seed, iteration);
            }
        }

        return placed;
    }

    // Write assembled jigsaw pieces into the world.
    template<class Write>
    int write_jigsaw_pieces(const std::vector<PlacedPiece>& pieces,
        std::int32_t chunk_x, std::int32_t chunk_z,
        Write&& write) {
        int placed = 0;
        for (const auto& piece : pieces) {
            for (const auto& cell : piece.tmpl.cells) {
                if (cell.state == 0) continue;  // Skip air
                const std::int32_t wx = piece.origin_x + cell.x;
                const std::int32_t wy = piece.origin_y + cell.y;
                const std::int32_t wz = piece.origin_z + cell.z;
                if ((wx >> 4) != chunk_x || (wz >> 4) != chunk_z) continue;
                write(wx, wy, wz, cell.state);
                ++placed;
            }
        }
        return placed;
    }

} // namespace terila::world::worldgen