#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace terila::data {

    struct DimensionInfo {
        std::int32_t min_y = -64;
        std::int32_t height = 384;
        std::int32_t logical_height = 384;
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
        std::unordered_map<std::string, std::int32_t> biome_ids;
        std::unordered_map<std::string, std::int32_t> entity_type_ids;
        std::unordered_map<std::string, std::string>  entity_categories;
        std::unordered_map<std::string, std::int32_t> item_ids;
        DimensionInfo overworld;

    };

}