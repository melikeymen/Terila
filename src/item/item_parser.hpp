#pragma once

#include "item/components.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace terila::item {

    struct ParsedItem {
        std::string id;          // "minecraft:diamond_sword"
        Components components;   // the patch written after the id
    };

    // The item argument of /give, in the same shape the client accepts:
    //   diamond_sword
    //   diamond_sword[minecraft:enchantments={levels:{"minecraft:sharpness":5}}]
    //   diamond_sword[minecraft:unbreakable={},!minecraft:damage]
    // The value of every component is stringified NBT, exactly as the game writes it.
    std::optional<ParsedItem> parse_item(std::string_view text, const ComponentIds& ids,
        std::string& error);

    // "diamond_sword" and "minecraft:diamond_sword" both mean the vanilla item.
    std::string namespaced(std::string_view name);

} // namespace terila::item
