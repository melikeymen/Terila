#pragma once

// The dimensions the game ships: what each one calls itself, how tall it is, whether it has
// a sky or a ceiling, and which noise settings and biome source feed it. The numbers come
// from worldgen/dimensions.json, so a build against another release follows that release
// instead of carrying a guessed height.

#include "data/input_store.hpp"
#include "world/dimension_kind.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    using DimensionKind = terila::world::DimensionKind;

    struct DimensionType {
        DimensionKind kind = DimensionKind::Overworld;
        std::string name = "minecraft:overworld";
        int min_y = -64;
        int height = 384;
        int logical_height = 384;
        bool has_skylight = true;
        bool has_ceiling = false;
        bool dragon_fight = false;
        double coordinate_scale = 1.0;
        double ambient_light = 0.0;
        std::string infiniburn;
    };

    inline const char* dimension_name(DimensionKind kind) {
        switch (kind) {
            case DimensionKind::Nether: return "minecraft:the_nether";
            case DimensionKind::End:    return "minecraft:the_end";
            default:                    return "minecraft:overworld";
        }
    }

    // The noise settings preset a dimension generates from.
    inline const char* dimension_noise_preset(DimensionKind kind) {
        switch (kind) {
            case DimensionKind::Nether: return "minecraft:nether";
            case DimensionKind::End:    return "minecraft:end";
            default:                    return "minecraft:overworld";
        }
    }

    // The multi-noise parameter table of a dimension. The End has no parameter table: its
    // biomes are picked from the island noise, not from a climate box.
    inline const char* dimension_biome_preset(DimensionKind kind) {
        switch (kind) {
            case DimensionKind::Nether: return "minecraft/nether";
            case DimensionKind::End:    return "";
            default:                    return "minecraft/overworld";
        }
    }

    inline DimensionType default_dimension_type(DimensionKind kind) {
        DimensionType type;
        type.kind = kind;
        type.name = dimension_name(kind);
        switch (kind) {
            case DimensionKind::Nether:
                type.min_y = 0;
                type.height = 256;
                type.logical_height = 128;
                type.has_skylight = false;
                type.has_ceiling = true;
                type.coordinate_scale = 8.0;
                type.ambient_light = 0.1;
                type.infiniburn = "#minecraft:infiniburn_nether";
                break;
            case DimensionKind::End:
                type.min_y = 0;
                type.height = 256;
                type.logical_height = 256;
                type.dragon_fight = true;
                type.ambient_light = 0.25;
                type.infiniburn = "#minecraft:infiniburn_end";
                break;
            default:
                break;
        }
        return type;
    }

    // Reads the three dimensions out of the extracted data. A missing file falls back to the
    // documented shape of each dimension so a partial input tree still generates.
    inline std::array<DimensionType, 3> load_dimension_types() {
        std::array<DimensionType, 3> types{
            default_dimension_type(DimensionKind::Overworld),
            default_dimension_type(DimensionKind::Nether),
            default_dimension_type(DimensionKind::End),
        };

        std::string text;
        if (!data::input::read_text("worldgen/dimensions.json", text)) {
            return types;
        }
        try {
            const nlohmann::json parsed = nlohmann::json::parse(text);
            for (const auto& entry : parsed.at("data")) {
                const std::string id = entry.at("id").get<std::string>();
                DimensionKind kind;
                if (id == "minecraft:the_nether") {
                    kind = DimensionKind::Nether;
                } else if (id == "minecraft:the_end") {
                    kind = DimensionKind::End;
                } else if (id == "minecraft:overworld") {
                    kind = DimensionKind::Overworld;
                } else {
                    continue;
                }
                const nlohmann::json& data = entry.at("data");
                DimensionType& type = types[static_cast<std::size_t>(kind)];
                type.name = id;
                type.min_y = data.value("min_y", type.min_y);
                type.height = data.value("height", type.height);
                type.logical_height = data.value("logical_height", type.height);
                type.has_skylight = data.value("has_skylight", type.has_skylight);
                type.has_ceiling = data.value("has_ceiling", type.has_ceiling);
                type.dragon_fight =
                    data.value("has_ender_dragon_fight", type.dragon_fight);
                type.coordinate_scale = data.value("coordinate_scale", type.coordinate_scale);
                type.ambient_light = data.value("ambient_light", type.ambient_light);
                type.infiniburn = data.value("infiniburn", type.infiniburn);
            }
        } catch (const std::exception&) {
        }
        return types;
    }

    // The id a command names, e.g. `execute in minecraft:the_nether run tp`. Short names
    // are accepted because a command never needs the namespace for the three shipped
    // dimensions.
    inline bool dimension_kind_of(std::string_view name, DimensionKind& kind) {
        if (name == "minecraft:overworld" || name == "overworld") {
            kind = DimensionKind::Overworld;
            return true;
        }
        if (name == "minecraft:the_nether" || name == "the_nether" || name == "nether") {
            kind = DimensionKind::Nether;
            return true;
        }
        if (name == "minecraft:the_end" || name == "the_end" || name == "end") {
            kind = DimensionKind::End;
            return true;
        }
        return false;
    }

    // The registry order the dump ships: overworld, overworld_caves, the_nether, the_end.
    // The client reads the same list during configuration, so the index has to match it.
    inline std::int32_t dimension_type_id(DimensionKind kind) {
        switch (kind) {
            case DimensionKind::Nether: return 2;
            case DimensionKind::End:    return 3;
            default:                    return 0;
        }
    }

    inline DimensionType dimension_type_of(DimensionKind kind) {
        static const std::array<DimensionType, 3> types = load_dimension_types();
        return types[static_cast<std::size_t>(kind)];
    }

} // namespace terila::world::worldgen
