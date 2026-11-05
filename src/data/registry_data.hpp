#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace terila::data {

	struct RegistryData {
		
		std::unordered_map<std::string, std::vector<std::uint8_t>> nbt_payloads;
		std::vector<std::uint8_t> tag_payload;
		std::vector<std::uint8_t> recipe_payload;
		std::vector<std::uint8_t> loot_payload;
		std::vector<std::uint8_t> advancement_payload;
		std::vector<std::uint8_t> command_tree_payload;

	};

}