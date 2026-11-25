#pragma once

// Jigsaw structure assembly: a structure starts from a template pool, each piece
// exposes jigsaw blocks that name a target pool and a target jigsaw name. The
// game picks a piece from that pool whose jigsaw block has the matching name,
// aligns the two pieces so the jigsaw blocks face each other, and replaces the
// jigsaw blocks with their final_state (usually air).

#include "data/input_store.hpp"
#include "world/worldgen/structures/placement.hpp"
#include "world/worldgen/structures/template.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <map>
#include <mutex>
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
        std::string joint;          // "aligned" or "rollable"
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
        const std::size_t start = orient_pos + std::string_view("orientation=").size();
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
        // A terrain-matching piece follows the ground under it; a rigid one keeps the
        // height its connector gave it.
        bool terrain_matching = false;
        // Quarter turns clockwise the template is laid down with, so the piece faces the
        // jigsaw it joins. The cells stay as the template holds them and are turned when
        // they are written.
        int rotation = 0;
        std::vector<JigsawBlock> jigsaws;  // Jigsaws in world coordinates
    };

    inline std::int32_t piece_size_x(const PlacedPiece& piece) {
        return rotated_size_x(piece.rotation, piece.tmpl.size_x, piece.tmpl.size_z);
    }

    inline std::int32_t piece_size_z(const PlacedPiece& piece) {
        return rotated_size_z(piece.rotation, piece.tmpl.size_x, piece.tmpl.size_z);
    }

    // A quarter turn clockwise on a jigsaw orientation: the side it faces turns with the
    // piece while up and down stay on the vertical axis.
    inline JigsawOrientation rotate_orientation(JigsawOrientation orientation,
        int quarter_turns) {
        const auto turn = [](JigsawOrientation side) {
            switch (side) {
                case JigsawOrientation::NorthUp: return JigsawOrientation::EastUp;
                case JigsawOrientation::EastUp: return JigsawOrientation::SouthUp;
                case JigsawOrientation::SouthUp: return JigsawOrientation::WestUp;
                case JigsawOrientation::WestUp: return JigsawOrientation::NorthUp;
                case JigsawOrientation::UpNorth: return JigsawOrientation::UpEast;
                case JigsawOrientation::UpEast: return JigsawOrientation::UpSouth;
                case JigsawOrientation::UpSouth: return JigsawOrientation::UpWest;
                case JigsawOrientation::UpWest: return JigsawOrientation::UpNorth;
                case JigsawOrientation::DownNorth: return JigsawOrientation::DownEast;
                case JigsawOrientation::DownEast: return JigsawOrientation::DownSouth;
                case JigsawOrientation::DownSouth: return JigsawOrientation::DownWest;
                case JigsawOrientation::DownWest: return JigsawOrientation::DownNorth;
                default: return side;
            }
        };
        JigsawOrientation turned = orientation;
        for (int index = 0; index < (quarter_turns & 3); ++index) {
            turned = turn(turned);
        }
        return turned;
    }

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
        // Several workers may be the first to ask for a pool at the same time, and a map that
        // is being filled while another thread reads it hangs or crashes. The file is read
        // once, and the map is only read afterwards.
        static std::once_flag once;
        std::call_once(once, [&] {
            std::string text;
            if (!data::input::read_text("worldgen/template_pools.json", text)) {
                return;
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
        });
        const auto found = cache.find(pool_id);
        if (found != cache.end()) {
            return found->second;
        }
        return std::nullopt;
    }

    // ---------------------------------------------------------------------------------
    // Processor lists: the worldgen rules that weather a placed piece (cracked bricks,
    // mossy cobble, rot). A piece names a list in its pool element; the list runs over the
    // template cells before they are written to the world.
    // ---------------------------------------------------------------------------------

    struct RuleEntry {
        enum class InputType {
            AlwaysTrue,
            BlockMatch,
            RandomBlockMatch,
            TagMatch
        } input = InputType::AlwaysTrue;

        std::int32_t input_state = -1;              // BlockMatch / RandomBlockMatch
        std::vector<std::int32_t> input_states;     // TagMatch expansion
        double probability = 1.0;                   // RandomBlockMatch
        std::int32_t output_state = 0;
    };

    struct ProcessorList {
        std::vector<RuleEntry> rules;
        bool has_rot = false;
        double integrity = 1.0;
        bool has_rot_table = false;
        std::vector<std::int32_t> rot_table;
        std::vector<std::int32_t> protected_states;
    };

    inline bool state_in(const std::vector<std::int32_t>& states, std::int32_t state) {
        for (const std::int32_t value : states) {
            if (value == state) {
                return true;
            }
        }
        return false;
    }

    // A uniform draw in [0, 1) from an LCG state.
    inline double unit_draw(std::uint64_t& state) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    }

    // The block tags (`#minecraft:x` in a rule) as the block names they list. Loaded once;
    // this is the same table the biome tags come from, under the block registry.
    inline const std::unordered_map<std::string, std::vector<std::string>>& block_tags() {
        static const std::unordered_map<std::string, std::vector<std::string>> table = [] {
            std::unordered_map<std::string, std::vector<std::string>> loaded;
            std::string text;
            if (!data::input::read_text("tags/all.json", text)) {
                return loaded;
            }
            try {
                const nlohmann::json parsed = nlohmann::json::parse(text);
                for (const nlohmann::json& registry : parsed.at("data")) {
                    if (registry.value("registry", std::string()) != "minecraft:block") {
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
            } catch (const std::exception&) {
            }
            return loaded;
        }();
        return table;
    }

    // Expands a `#tag` into the states the named blocks default to.
    inline std::vector<std::int32_t> resolve_tag(const std::string& tag,
        const StateTextResolver& resolve) {
        std::vector<std::int32_t> states;
        std::string name = tag;
        if (!name.empty() && name.front() == '#') {
            name.erase(0, 1);
        }
        const auto found = block_tags().find(name);
        if (found == block_tags().end()) {
            return states;
        }
        for (const std::string& block : found->second) {
            const std::int32_t state = resolve(block);
            if (state >= 0) {
                states.push_back(state);
            }
        }
        return states;
    }

    // A processor names a state either as a plain string or as `{id, properties}`; both
    // forms become the `name[key=value,...]` text the resolver already reads.
    inline std::string state_text(const nlohmann::json& value) {
        if (value.is_string()) {
            return value.get<std::string>();
        }
        if (!value.is_object()) {
            return "minecraft:air";
        }
        std::string text = value.value("id", std::string("minecraft:air"));
        const auto properties = value.find("properties");
        if (properties != value.end() && properties->is_object()) {
            std::map<std::string, std::string> ordered;
            for (const auto& [key, property] : properties->items()) {
                ordered.emplace(key,
                    property.is_string() ? property.get<std::string>() : property.dump());
            }
            text.push_back('[');
            bool first = true;
            for (const auto& [key, property] : ordered) {
                if (!first) {
                    text.push_back(',');
                }
                first = false;
                text += key;
                text.push_back('=');
                text += property;
            }
            text.push_back(']');
        }
        return text;
    }

    // The state a predicate matches: newer lists name a full state under `block_state`.
    inline std::int32_t predicate_state(const nlohmann::json& predicate,
        const StateTextResolver& resolve) {
        if (const auto state = predicate.find("block_state"); state != predicate.end()) {
            return resolve(state_text(*state));
        }
        if (const auto block = predicate.find("block"); block != predicate.end()) {
            return resolve(state_text(*block));
        }
        return -1;
    }

    // Compiles one processor list against a state resolver. Location-sensitive rules
    // (water under a path, axis-aligned decay) cannot be judged from the template alone,
    // so they are skipped rather than applied everywhere.
    inline std::optional<ProcessorList> load_processor_list(const std::string& id,
        const StateTextResolver& resolve) {
        // Read once for the whole process, like the pools: generation asks for lists from
        // several threads.
        static std::optional<nlohmann::json> file;
        static std::once_flag once;
        static bool failed = false;
        std::call_once(once, [&] {
            std::string text;
            if (!data::input::read_text("worldgen/processor_lists.json", text)) {
                failed = true;
                return;
            }
            try {
                file = nlohmann::json::parse(text);
            } catch (const std::exception&) {
                failed = true;
            }
        });
        if (failed) {
            return std::nullopt;
        }
        if (!file) {
            return std::nullopt;
        }

        for (const auto& entry : file->at("data")) {
            if (entry.at("id").get<std::string>() != id) {
                continue;
            }
            ProcessorList list;
            if (!entry.contains("data") || !entry.at("data").contains("processors")) {
                return list;
            }
            for (const auto& processor : entry.at("data").at("processors")) {
                const std::string type = processor.value("processor_type", "");
                if (type == "minecraft:block_rot") {
                    list.has_rot = true;
                    list.integrity = processor.value("integrity", 1.0);
                    if (processor.contains("rottable_blocks")) {
                        list.has_rot_table = true;
                        list.rot_table = resolve_tag(
                            processor.at("rottable_blocks").get<std::string>(), resolve);
                    }
                } else if (type == "minecraft:protected_blocks") {
                    if (processor.contains("value")) {
                        const auto states = resolve_tag(
                            processor.at("value").get<std::string>(), resolve);
                        list.protected_states.insert(list.protected_states.end(),
                            states.begin(), states.end());
                    }
                } else if (type == "minecraft:rule") {
                    for (const auto& rule : processor.at("rules")) {
                        const auto location = rule.find("location_predicate");
                        if (location != rule.end() &&
                            location->value("predicate_type", "") != "minecraft:always_true") {
                            continue;
                        }
                        if (rule.contains("position_predicate")) {
                            continue;
                        }
                        RuleEntry compiled;
                        const nlohmann::json& predicate = rule.at("input_predicate");
                        const std::string predicate_type =
                            predicate.value("predicate_type", "minecraft:always_true");
                        if (predicate_type == "minecraft:block_match") {
                            compiled.input = RuleEntry::InputType::BlockMatch;
                            compiled.input_state = predicate_state(predicate, resolve);
                        } else if (predicate_type == "minecraft:random_block_match") {
                            compiled.input = RuleEntry::InputType::RandomBlockMatch;
                            compiled.input_state = predicate_state(predicate, resolve);
                            compiled.probability = predicate.value("probability", 0.0);
                        } else if (predicate_type == "minecraft:tag_match") {
                            compiled.input = RuleEntry::InputType::TagMatch;
                            compiled.input_states = resolve_tag(
                                predicate.value("tag", ""), resolve);
                            if (compiled.input_states.empty()) {
                                continue;
                            }
                        } else {
                            compiled.input = RuleEntry::InputType::AlwaysTrue;
                        }
                        compiled.output_state = rule.contains("output_state")
                            ? resolve(state_text(rule.at("output_state")))
                            : 0;
                        list.rules.push_back(std::move(compiled));
                    }
                }
            }
            return list;
        }
        return std::nullopt;
    }

    // Runs a compiled list over the template's cells. Rot removes blocks first, then the
    // rules rewrite what is left; a rule that matches ends the cell.
    inline void apply_processors(StructureTemplate& tmpl, const ProcessorList& list,
        std::uint64_t& rng) {
        if (list.has_rot) {
            for (TemplateCell& cell : tmpl.cells) {
                if (cell.state == 0) {
                    continue;
                }
                if (state_in(list.protected_states, cell.state)) {
                    continue;
                }
                if (list.has_rot_table && !state_in(list.rot_table, cell.state)) {
                    continue;
                }
                if (unit_draw(rng) >= list.integrity) {
                    cell.state = 0;
                }
            }
        }
        for (TemplateCell& cell : tmpl.cells) {
            if (cell.state == 0) {
                continue;
            }
            for (const RuleEntry& rule : list.rules) {
                bool matches = false;
                switch (rule.input) {
                    case RuleEntry::InputType::AlwaysTrue:
                        matches = true;
                        break;
                    case RuleEntry::InputType::BlockMatch:
                        matches = cell.state == rule.input_state;
                        break;
                    case RuleEntry::InputType::RandomBlockMatch:
                        matches = cell.state == rule.input_state &&
                            unit_draw(rng) < rule.probability;
                        break;
                    case RuleEntry::InputType::TagMatch:
                        matches = state_in(rule.input_states, cell.state);
                        break;
                }
                if (matches) {
                    cell.state = rule.output_state;
                    break;
                }
            }
        }
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
        // Generation fills this cache from several workers at once, so every read and write
        // of it is under the lock. A template is parsed outside the lock; the insert keeps
        // the copy that won.
        static std::mutex mutex;

        // The mounted input root is part of the key: a test that points the store at a
        // different directory must not read the templates of another one.
        const std::string key = data::input::root() + '|' + std::string(id);
        {
            const std::lock_guard lock(mutex);
            const auto cached = cache.find(key);
            if (cached != cache.end()) {
                out_tmpl = cached->second.first;
                out_jigsaws = cached->second.second;
                return true;
            }
            if (negative_cache.find(key) != negative_cache.end()) {
                return false;
            }
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
                    const std::int32_t state = entry.key().find("minecraft:structure_void") == 0
                        ? kStructureVoid : resolve(entry.key());
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
            // A jigsaw block is scaffolding: the game replaces it with the state the block
            // entity names as its final state as soon as the piece is placed, so the piece
            // itself carries that state from here on.
            for (const JigsawBlock& jb : jigsaws) {
                // A final state of structure void means the joint leaves the terrain alone, so
                // the cell is skipped the same way a void cell of the palette is.
                const std::int32_t final_state =
                    jb.final_state.find("minecraft:structure_void") != std::string::npos
                        ? kStructureVoid : resolve(jb.final_state);
                for (TemplateCell& cell : tmpl.cells) {
                    if (cell.x == jb.x && cell.y == jb.y && cell.z == jb.z) {
                        cell.state = final_state;
                        break;
                    }
                }
            }
            out_tmpl = std::move(tmpl);
            out_jigsaws = std::move(jigsaws);
            const std::lock_guard lock(mutex);
            cache.emplace(key, std::make_pair(out_tmpl, out_jigsaws));
            return true;
        } catch (const std::exception&) {
            const std::lock_guard lock(mutex);
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
        std::uint64_t& rng, int rotation = 0) {
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

                // A pool element's processors weather the template before it is placed.
                if (!elem.processors.empty() && elem.processors != "inline") {
                    if (auto list = load_processor_list(elem.processors, resolve)) {
                        apply_processors(tmpl, *list, rng);
                    }
                }

                const std::int32_t size_x = tmpl.size_x;
                const std::int32_t size_z = tmpl.size_z;
                PlacedPiece piece;
                piece.tmpl = std::move(tmpl);
                piece.origin_x = origin_x;
                piece.origin_y = origin_y;
                piece.origin_z = origin_z;
                piece.terrain_matching = elem.projection == "terrain_matching";
                piece.rotation = rotation & 3;
                // A jigsaw turns with its piece, so its connection is made where the turn
                // puts it.
                for (auto& jb : jigsaws) {
                    std::int32_t local_x = jb.x;
                    std::int32_t local_z = jb.z;
                    rotate_cell(piece.rotation, size_x, size_z, local_x, local_z);
                    jb.x = local_x + origin_x;
                    jb.y += origin_y;
                    jb.z = local_z + origin_z;
                }
                piece.jigsaws = std::move(jigsaws);
                placed.push_back(std::move(piece));
                return true;
            }
            case PoolElement::Type::List: {
                for (const auto& child : elem.elements) {
                    if (!place_pool_element(child, resolve, placed, pending,
                            origin_x, origin_y, origin_z, rng, rotation)) {
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
        std::int32_t size_x = 0;
        std::int32_t size_z = 0;
    };

    // Every candidate for (pool, jigsaw name), built once per pair. The pool scan reads
    // whole templates, so caching it keeps a large structure from re-reading the village
    // houses for every street it grows.
    inline const std::vector<JigsawCandidate>& candidates_for(const std::string& pool_id,
        const std::string& target_name, const StateTextResolver& resolve) {
        // A std::map keeps the address of an entry stable while another worker inserts the
        // next pool, so the reference handed back stays valid after the lock is dropped. The
        // scan itself runs outside the lock: two workers that need the same pool may scan it
        // twice, but neither waits on the other's template loading.
        static std::map<std::string, std::vector<JigsawCandidate>> cache;
        static std::mutex mutex;
        const std::string key = pool_id + '\x1f' + target_name;
        {
            const std::lock_guard lock(mutex);
            const auto cached = cache.find(key);
            if (cached != cache.end()) {
                return cached->second;
            }
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
                            list.push_back({ elem, jb, tmpl.size_x, tmpl.size_z });
                        }
                    }
                }
            }
        }
        const std::lock_guard lock(mutex);
        return cache.try_emplace(key, std::move(list)).first->second;
    }

    // Try to connect a pending jigsaw by picking a piece from its target pool.
    inline bool try_connect_jigsaw(const JigsawConnection& conn,
        const StateTextResolver& resolve,
        std::vector<PlacedPiece>& placed,
        std::vector<JigsawConnection>& new_pending,
        std::uint64_t& rng,
        int depth, std::int32_t centre_x, std::int32_t centre_z, int max_distance,
        bool allow_rotation) {
        if (depth > 50) return false;  // Prevent infinite recursion
        if (conn.target_pool.empty()) return true;

        const std::vector<JigsawCandidate>& candidates =
            candidates_for(conn.target_pool, conn.target_name, resolve);
        if (candidates.empty()) {
            // No matching piece; fall back to empty (which does nothing).
            return true;
        }

        // The draw picks the first candidate; when it does not fit, the rest of the pool is
        // tried before the connection is given up, so a street is not lost to a bad draw.
        const std::size_t picked = static_cast<std::size_t>(rng % candidates.size());
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        // Both joint types join here. An aligned joint asks for two blocks that face each
        // other with the same top, and a rollable one lets the new piece be rolled around
        // the connector; the roll is a quarter turn of the piece, which is what the turn
        // search below lays down anyway. A building's `bottom` jigsaws are rollable, and
        // they are what hangs a well or a villager under the start piece.
        const JigsawOrientation needed_orient = opposite(conn.orientation);

        for (std::size_t attempt = 0; attempt < candidates.size(); ++attempt) {
            const JigsawCandidate& chosen =
                candidates[(picked + attempt) % candidates.size()];
            const PoolElement& chosen_elem = chosen.elem;
            const JigsawBlock& matching_jigsaw = chosen.jigsaw;

            // The piece's jigsaw must land on the connection and face it. A piece may be laid
            // down with a quarter turn, so the turn that puts its jigsaw right is the one
            // used.
            int rotation = 0;
            bool aligned = false;
            std::int32_t new_origin_x = 0;
            std::int32_t new_origin_y = 0;
            std::int32_t new_origin_z = 0;
            for (; rotation < (allow_rotation ? 4 : 1); ++rotation) {
                std::int32_t local_x = matching_jigsaw.x;
                std::int32_t local_z = matching_jigsaw.z;
                rotate_cell(rotation, chosen.size_x, chosen.size_z, local_x, local_z);
                const JigsawOrientation piece_orient = rotate_orientation(
                    parse_orientation(matching_jigsaw.orientation), rotation);
                if (piece_orient != needed_orient) {
                    continue;
                }
                new_origin_x = conn.x - local_x;
                new_origin_y = conn.y - matching_jigsaw.y;
                new_origin_z = conn.z - local_z;
                aligned = true;
                break;
            }
            if (!aligned) {
                continue;
            }

            // Place the candidate at the computed origin, turned to face the connection.
            const std::size_t placed_before = placed.size();
            if (!place_pool_element(chosen_elem, resolve, placed, new_pending,
                    new_origin_x, new_origin_y, new_origin_z, rng, rotation)) {
                continue;
            }
            if (placed.size() == placed_before) {
                continue;  // Nothing landed, so there is nothing to expand from.
            }

            // A structure may only grow as far from its centre as its data allows, so a long
            // chain of streets cannot run off into the world.
            bool within = true;
            if (max_distance > 0) {
                const std::int64_t limit =
                    static_cast<std::int64_t>(max_distance) * max_distance;
                for (std::size_t index = placed_before; index < placed.size(); ++index) {
                    const PlacedPiece& piece = placed[index];
                    const std::int64_t dx = piece.origin_x + piece_size_x(piece) / 2 - centre_x;
                    const std::int64_t dz = piece.origin_z + piece_size_z(piece) / 2 - centre_z;
                    if (dx * dx + dz * dz > limit) {
                        within = false;
                        break;
                    }
                }
            }
            if (!within) {
                placed.resize(placed_before);  // The whole connection is dropped.
                continue;
            }

            // The new piece's jigsaws (except the one we just connected) become new pending
            // connections.
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
        return false;
    }

    // Assemble a jigsaw structure starting from a start pool.
    // Returns the placed pieces. The start piece is placed at the given origin.
    // `max_pieces` bounds the village/street expansion, which in vanilla is bounded by
    // the structure's `size` field: an unbounded breadth-first walk would grow until the
    // memory runs out.
    inline std::vector<PlacedPiece> assemble_jigsaw(const std::string& start_pool_id,
        const StateTextResolver& resolve,
        std::int32_t origin_x, std::int32_t origin_y, std::int32_t origin_z,
        std::uint64_t seed, int max_pieces = 64, int max_depth = 200, int max_distance = 0,
        const StateRotator& rotate = {}) {
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

        // Resolve pending connections (breadth-first, bounded by the piece cap). One
        // iteration grows the structure by one layer, so the structure's size caps the
        // number of layers it may grow.
        for (int iteration = 0; iteration < max_depth && !pending.empty() &&
            static_cast<int>(placed.size()) < max_pieces; ++iteration) {
            std::vector<JigsawConnection> current = std::move(pending);
            pending.clear();
            for (const auto& conn : current) {
                if (static_cast<int>(placed.size()) >= max_pieces) break;
                try_connect_jigsaw(conn, resolve, placed, pending, seed, iteration,
                    origin_x, origin_z, max_distance, static_cast<bool>(rotate));
            }
        }

        return placed;
    }

    // Write assembled jigsaw pieces into the world. A terrain-matching piece drops onto the
    // height under its own footprint, and the gap a piece leaves over a slope is filled with
    // the given filler, so a building always meets the ground.
    template<class Surface, class Write>
    int write_jigsaw_pieces(const std::vector<PlacedPiece>& pieces,
        std::int32_t chunk_x, std::int32_t chunk_z,
        Surface&& surface, std::int32_t filler, const StateRotator& rotate, Write&& write) {
        // A turned piece owns the turned footprint, so its ground and its blocks follow the
        // turn as well.
        const auto base_of = [&surface](const PlacedPiece& piece) {
            if (!piece.terrain_matching) {
                return piece.origin_y;
            }
            return surface(piece.origin_x + piece_size_x(piece) / 2,
                piece.origin_z + piece_size_z(piece) / 2);
        };
        const auto state_of = [&rotate](std::int32_t state, int rotation) {
            return rotate ? rotate(state, rotation) : state;
        };

        // The beards first: every column a piece covers is raised to its floor, so a house
        // on a slope stands on filled ground instead of hanging over it. Filling before the
        // blocks are written keeps one piece's beard from covering another's floor.
        for (const auto& piece : pieces) {
            const std::int32_t base_y = base_of(piece);
            for (std::int32_t wz = piece.origin_z;
                wz < piece.origin_z + piece_size_z(piece); ++wz) {
                if ((wz >> 4) != chunk_z) continue;
                for (std::int32_t wx = piece.origin_x;
                    wx < piece.origin_x + piece_size_x(piece); ++wx) {
                    if ((wx >> 4) != chunk_x) continue;
                    const std::int32_t ground = surface(wx, wz);
                    if (base_y - 1 > ground + kMaxBeard) continue;
                    for (std::int32_t wy = ground + 1; wy < base_y; ++wy) {
                        write(wx, wy, wz, filler);
                    }
                }
            }
        }

        int placed = 0;
        for (const auto& piece : pieces) {
            const std::int32_t base_y = base_of(piece);
            for (const auto& cell : piece.tmpl.cells) {
                if (cell.state == kStructureVoid) continue;  // The terrain keeps this cell
                std::int32_t local_x = cell.x;
                std::int32_t local_z = cell.z;
                rotate_cell(piece.rotation, piece.tmpl.size_x, piece.tmpl.size_z,
                    local_x, local_z);
                const std::int32_t wx = piece.origin_x + local_x;
                const std::int32_t wy = base_y + cell.y;
                const std::int32_t wz = piece.origin_z + local_z;
                if ((wx >> 4) != chunk_x || (wz >> 4) != chunk_z) continue;
                write(wx, wy, wz, state_of(cell.state, piece.rotation));
                ++placed;
            }
        }
        return placed;
    }

} // namespace terila::world::worldgen