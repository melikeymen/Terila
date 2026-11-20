#pragma once

// Structure templates: a template is the palette of block states a structure is built from,
// plus the cells each state fills. The game keeps them as NBT files; the extraction turns
// every one into JSON (`input/structures/templates/<name>.json`) whose palette names each
// state in full, so loading one is reading the size and the cells of its palette.

#include "data/input_store.hpp"
#include "data/registry_data.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terila::world::worldgen {

    struct TemplateCell {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        std::int32_t state = 0;
    };

    struct StructureTemplate {
        std::int32_t size_x = 0;
        std::int32_t size_y = 0;
        std::int32_t size_z = 0;
        std::vector<TemplateCell> cells;

        bool empty() const { return cells.empty(); }
    };

    // "minecraft:stone_brick_stairs[facing=east,half=bottom]" -> the number the world stores
    // for that state. A state the registry does not carry falls back to the block's default
    // state and, when even that is missing, to air.
    inline std::int32_t template_state_id(std::string_view text,
        const std::unordered_map<std::string, std::vector<data::BlockStateInfo>>& block_states,
        const std::unordered_map<std::string, std::int32_t>& defaults) {
        const std::size_t bracket = text.find('[');
        const std::string_view name =
            bracket == std::string_view::npos ? text : text.substr(0, bracket);
        const std::string block(name);
        if (bracket == std::string_view::npos) {
            if (const auto single = defaults.find(block); single != defaults.end()) {
                return single->second;
            }
            return 0;
        }

        std::vector<std::pair<std::string_view, std::string_view>> wanted;
        std::size_t at = bracket + 1;
        while (at < text.size()) {
            const std::size_t end = text.find_first_of(",]", at);
            if (end == std::string_view::npos) {
                break;
            }
            const std::string_view pair = text.substr(at, end - at);
            const std::size_t equals = pair.find('=');
            if (equals != std::string_view::npos) {
                wanted.emplace_back(pair.substr(0, equals), pair.substr(equals + 1));
            }
            at = end + 1;
        }

        if (const auto known = block_states.find(block); known != block_states.end()) {
            for (const data::BlockStateInfo& variant : known->second) {
                if (variant.properties.size() != wanted.size()) {
                    continue;
                }
                bool matches = true;
                for (const auto& [key, value] : wanted) {
                    const auto property = variant.properties.find(std::string(key));
                    if (property == variant.properties.end() || property->second != value) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    return variant.id;
                }
            }
        }

        if (const auto single = defaults.find(block); single != defaults.end()) {
            return single->second;
        }
        return 0;
    }

    // The extracted form of a template, keyed by the template id the structure data names.
    inline std::string template_path(std::string_view id) {
        std::string name(id);
        if (name.rfind("minecraft:", 0) == 0) {
            name.erase(0, 10);
        }
        return "structures/templates/minecraft/" + name + ".json";
    }

    // Turns a state string into the number the world stores; the generator has no registry
    // at hand and answers with the block's default state instead.
    using StateTextResolver = std::function<std::int32_t(std::string_view)>;

    inline bool load_template(std::string_view id, const StateTextResolver& resolve,
        StructureTemplate& out) {
        out = StructureTemplate{};
        std::string text;
        if (!data::input::read_text(template_path(id), text)) {
            return false;
        }

        try {
            const nlohmann::json parsed = nlohmann::json::parse(text);
            const nlohmann::json& size = parsed.at("size");
            out.size_x = size.at("x").get<std::int32_t>();
            out.size_y = size.at("y").get<std::int32_t>();
            out.size_z = size.at("z").get<std::int32_t>();

            const auto blocks = parsed.find("blocks");
            if (blocks == parsed.end() || !blocks->is_object()) {
                return true;
            }
            for (auto entry = blocks->begin(); entry != blocks->end(); ++entry) {
                const std::int32_t state = resolve(entry.key());
                for (const nlohmann::json& position : entry.value()) {
                    if (!position.is_array() || position.size() < 3) {
                        continue;
                    }
                    TemplateCell cell;
                    cell.x = position.at(0).get<std::int32_t>();
                    cell.y = position.at(1).get<std::int32_t>();
                    cell.z = position.at(2).get<std::int32_t>();
                    cell.state = state;
                    out.cells.push_back(cell);
                }
            }
        }
        catch (const std::exception&) {
            return false;
        }
        return true;
    }

    inline bool load_template(std::string_view id, const data::RegistryData& registry,
        StructureTemplate& out) {
        return load_template(id,
            [&registry](std::string_view text) {
                return template_state_id(text, registry.block_states, registry.block_state_ids);
            },
            out);
    }

} // namespace terila::world::worldgen
