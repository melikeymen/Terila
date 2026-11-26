#pragma once

#include "world/block_break.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace terila::data {

    // What the game reads from a block state for lighting and for walking into it, joined
    // to the state ids the chunk format carries.
    struct BlockLightInfo {
        std::uint8_t emission = 0;
        std::uint8_t dampening = 15;
        bool sky_down = false;
        bool collides = true;
    };

    struct DimensionInfo {
        std::int32_t min_y = -64;
        std::int32_t height = 384;
        std::int32_t logical_height = 384;
        // The darkness a monster needs to appear here: a range of light levels it tests the
        // raw brightness against, and the block light above which it never spawns.
        int monster_spawn_light_min = 0;
        int monster_spawn_light_max = 7;
        int monster_spawn_block_light_limit = 0;
    };

    // One variant of a block, as listed by blocks/state_ids.json.
    struct BlockStateInfo {
        std::int32_t id = -1;
        std::unordered_map<std::string, std::string> properties;
    };

    // What a block does when it is dug at, as far as breaking is concerned.
    struct BlockBreakInfo {
        float hardness = 0.0f;
        bool requires_correct_tool = false;
        // The block kind behind the state, so a tool's tag rules can be answered.
        std::string block;
    };

    struct RegistryData {

        std::unordered_map<std::string, std::vector<std::uint8_t>> nbt_payloads;
        std::vector<std::string> datapack_registries;
        std::vector<std::uint8_t> tag_payload;
        std::vector<std::uint8_t> recipe_payload;
        std::vector<std::uint8_t> loot_payload;
        std::vector<std::uint8_t> advancement_payload;
        std::vector<std::uint8_t> command_tree_payload;

        std::unordered_map<std::string, std::int32_t> block_state_ids;
        std::unordered_map<std::string, std::vector<BlockStateInfo>> block_states;
        // Per state: light emission, light dampening and the collision answer.
        std::unordered_map<std::int32_t, BlockLightInfo> block_light;
        // Keyed by state id because a dig names the block it hits, not the block kind.
        std::unordered_map<std::int32_t, BlockBreakInfo> block_break_info;
        // Which tags each block answers to, and what each tool item is good at.
        std::unordered_map<std::string, std::unordered_set<std::string>> block_tags;
        std::unordered_map<std::int32_t, std::vector<world::ToolRule>> tool_rules;
        std::unordered_map<std::string, std::int32_t> biome_ids;
        std::unordered_map<std::string, std::int32_t> entity_type_ids;
        std::unordered_map<std::string, std::string>  entity_categories;
        std::unordered_map<std::string, std::int32_t> item_ids;
        std::unordered_map<std::string, std::int32_t> component_ids;
        std::unordered_map<std::string, std::int32_t> enchantment_ids;
        // minecraft:command_argument_type, needed to describe arguments in the command tree.
        std::unordered_map<std::string, std::int32_t> command_argument_types;
        DimensionInfo overworld;

    };

}
