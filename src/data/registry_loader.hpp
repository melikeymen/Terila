#pragma once

#include "data/registry_data.hpp"
#include "protocol/varint.hpp"
#include "protocol/string.hpp"
#include "log/logger.hpp"

#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <exception>
#include <nlohmann/json.hpp>

namespace terila::data {

	inline bool load_binary(std::vector<std::uint8_t>& out, const std::string& path) {
		std::ifstream file(path, std::ios::binary);

		if (!file) {
			LOG_WARN(Data, "cannot open file: {}", path);
			return false;
		}

		out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

		if (out.empty()) {
			LOG_WARN(Data, "empty file: {}", path);
			return false;
		}

		LOG_TRACE(Data, "loaded {} bytes from {}", out.size(), path);
		return true;
	}

	inline bool load_command_tree(std::vector<std::uint8_t>& out, const std::string& path) {

		if (!load_binary(out, path)) {
			return false;
		}

		std::size_t i = 0;

		while (i < out.size() && (out[i] & 0x80)) ++i;

		if (i < out.size()) ++i;

		LOG_TRACE(Data, "stripping {} byte varint prefix from {}", i, path);
		out.erase(out.begin(), out.begin() + i);

		if (out.empty()) {
			LOG_WARN(Data, "empty after prefix strip: {}", path);
			return false;
		}

		LOG_DEBUG(Data, "command tree ready: {} bytes", out.size());
		return true;
	}

	// blocks/state_ids.json format:
	//   { data: [ { id: "minecraft:acacia_button", default_state_id: N,
	//               min_state_id: N, max_state_id: N, properties: {...} } ] }
	inline void load_block_state_ids(std::unordered_map<std::string, std::int32_t>& out, const std::string& path) {
		std::ifstream f(path);
		if (!f) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(f);

			if (!j.contains("data") || !j["data"].is_array()) {
				LOG_WARN(Data, "unexpected format in {} (no 'data' array)", path);
				return;
			}

			for (const auto& e : j["data"]) {
				if (!e.contains("id") || !e["id"].is_string()) continue;
				std::string name = e["id"].get<std::string>();

				std::int32_t id = -1;
				if (e.contains("default_state_id"))      id = e["default_state_id"].get<std::int32_t>();
				else if (e.contains("min_state_id"))     id = e["min_state_id"].get<std::int32_t>();
				if (id < 0) continue;

				if (out.find(name) == out.end()) out[name] = id;
			}
		}
		catch (const std::exception& e) {
			LOG_WARN(Data, "parse failed: {} ({})", path, e.what());
		}

		LOG_DEBUG(Data, "block_state_ids: {} entries", out.size());
	}

	// worldgen/biomes.json format:
	//   { data: [ { id: "minecraft:badlands", protocol_id: N, data: {...} } ] }
	inline void load_biome_ids(std::unordered_map<std::string, std::int32_t>& out, const std::string& path) {
		std::ifstream f(path);
		if (!f) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(f);

			if (!j.contains("data") || !j["data"].is_array()) {
				LOG_WARN(Data, "unexpected format in {} (no 'data' array)", path);
				return;
			}

			for (const auto& e : j["data"]) {
				if (!e.contains("id") || !e["id"].is_string()) continue;
				if (!e.contains("protocol_id")) continue;

				std::string name = e["id"].get<std::string>();
				std::int32_t id = e["protocol_id"].get<std::int32_t>();
				out[name] = id;
			}
		}
		catch (const std::exception& e) {
			LOG_WARN(Data, "parse failed: {} ({})", path, e.what());
		}

		LOG_DEBUG(Data, "biome_ids: {} entries", out.size());
	}

	// entities/minecraft.json format:
	//   { data: [ { id: "minecraft:allay", protocol_id: N, ... } ] }
	inline void load_entity_type_ids(std::unordered_map<std::string, std::int32_t>& out, const std::string& path) {
		std::ifstream f(path);
		if (!f) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(f);

			if (!j.contains("data") || !j["data"].is_array()) {
				LOG_WARN(Data, "unexpected format in {} (no 'data' array)", path);
				return;
			}

			for (const auto& e : j["data"]) {
				if (!e.contains("id") || !e["id"].is_string()) continue;
				if (!e.contains("protocol_id")) continue;

				out[e["id"].get<std::string>()] = e["protocol_id"].get<std::int32_t>();
			}
		}
		catch (const std::exception& e) {
			LOG_WARN(Data, "parse failed: {} ({})", path, e.what());
		}

		LOG_DEBUG(Data, "entity_type_ids: {} entries", out.size());
	}

	// items/minecraft.json format:
	//   { data: [ { id: "minecraft:stone", protocol_id: N, ... } ] }
	inline void load_item_ids(std::unordered_map<std::string, std::int32_t>& out, const std::string& path) {
		std::ifstream f(path);
		if (!f) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(f);

			if (!j.contains("data") || !j["data"].is_array()) {
				LOG_WARN(Data, "unexpected format in {} (no 'data' array)", path);
				return;
			}

			for (const auto& e : j["data"]) {
				if (!e.contains("id") || !e["id"].is_string()) continue;
				if (!e.contains("protocol_id")) continue;

				out[e["id"].get<std::string>()] = e["protocol_id"].get<std::int32_t>();
			}
		}
		catch (const std::exception& e) {
			LOG_WARN(Data, "parse failed: {} ({})", path, e.what());
		}

		LOG_DEBUG(Data, "item_ids: {} entries", out.size());
	}

	// Same file as the entity ids: category tells hostile mobs from passive ones.
	inline void load_entity_categories(std::unordered_map<std::string, std::string>& out, const std::string& path) {
		std::ifstream f(path);
		if (!f) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(f);

			if (!j.contains("data") || !j["data"].is_array()) {
				LOG_WARN(Data, "unexpected format in {} (no 'data' array)", path);
				return;
			}

			for (const auto& e : j["data"]) {
				if (!e.contains("id") || !e["id"].is_string()) continue;
				if (!e.contains("category") || !e["category"].is_string()) continue;

				out[e["id"].get<std::string>()] = e["category"].get<std::string>();
			}
		}
		catch (const std::exception& e) {
			LOG_WARN(Data, "parse failed: {} ({})", path, e.what());
		}

		LOG_DEBUG(Data, "entity_categories: {} entries", out.size());
	}

	// worldgen/dimensions.json format (best-effort):
	//   - Array of { id: "minecraft:overworld", min_y, height, logical_height }
	//   - OR object keyed by "minecraft:overworld" / "overworld"
	inline void load_dimensions(DimensionInfo& out, const std::string& path) {
		std::ifstream f(path);
		if (!f) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(f);
			nlohmann::json ow;

			if (j.contains("data") && j["data"].is_array()) {
				for (const auto& e : j["data"]) {
					if (e.contains("id") && e["id"].is_string() && e["id"] == "minecraft:overworld") {
						ow = e;
						break;
					}
				}
			}
			else if (j.is_array()) {
				for (const auto& e : j) {
					if (e.contains("id") && e["id"].is_string() && e["id"] == "minecraft:overworld") {
						ow = e;
						break;
					}
				}
			}
			else if (j.is_object()) {
				if (j.contains("minecraft:overworld"))      ow = j["minecraft:overworld"];
				else if (j.contains("overworld"))           ow = j["overworld"];
			}

			if (!ow.is_null()) {
				if (ow.contains("min_y"))          out.min_y = ow["min_y"].get<std::int32_t>();
				if (ow.contains("height"))         out.height = ow["height"].get<std::int32_t>();
				if (ow.contains("logical_height")) out.logical_height = ow["logical_height"].get<std::int32_t>();
			}
			else {
				LOG_WARN(Data, "overworld entry not found in {}", path);
			}
		}
		catch (const std::exception& e) {
			LOG_WARN(Data, "parse failed: {} ({})", path, e.what());
		}

		LOG_DEBUG(Data, "overworld: min_y={} height={}", out.min_y, out.height);
	}

}