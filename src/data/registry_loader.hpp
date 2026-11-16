#pragma once

#include "data/input_store.hpp"
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
#include <simdjson.h>

namespace terila::data {

	// Paths here are relative to the input root: the store decides whether that is a directory
	// or the table compiled into the binary.
	inline bool load_binary(std::vector<std::uint8_t>& out, const std::string& path) {
		if (!input::read(path, out)) {
			LOG_WARN(Data, "cannot open file: {}", path);
			return false;
		}

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

	// registries/dump/minecraft/data_component_type.json format:
	//   { registries: { entries: [ { id: "minecraft:enchantments", protocol_id: N } ] } }
	inline void load_data_component_ids(std::unordered_map<std::string, std::int32_t>& out, const std::string& path) {
		std::string text;
		if (!input::read_text(path, text)) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(text);
			if (!j.contains("registries") || !j["registries"].contains("entries")) {
				LOG_WARN(Data, "unexpected format in {} (no registries.entries)", path);
				return;
			}

			for (const auto& e : j["registries"]["entries"]) {
				if (!e.contains("id") || !e.contains("protocol_id")) continue;
				out[e["id"].get<std::string>()] = e["protocol_id"].get<std::int32_t>();
			}
		}
		catch (const std::exception& e) {
			LOG_WARN(Data, "parse failed: {} ({})", path, e.what());
		}

		LOG_DEBUG(Data, "data component ids: {} entries", out.size());
	}

	// The command tree has to name its arguments with the parser ids of this exact
	// version, so they are read from the registry dump instead of being hardcoded.
	inline void load_command_argument_types(std::unordered_map<std::string, std::int32_t>& out, const std::string& path) {
		load_data_component_ids(out, path);
	}

	inline void load_enchantment_ids(std::unordered_map<std::string, std::int32_t>& out, const std::string& path) {
		std::string text;
		if (!input::read_text(path, text)) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(text);
			if (!j.contains("registries") || !j["registries"].contains("entries")) {
				LOG_WARN(Data, "unexpected format in {} (no registries.entries)", path);
				return;
			}

			for (const auto& e : j["registries"]["entries"]) {
				if (!e.contains("id") || !e["id"].is_string()) continue;
				if (!e.contains("protocol_id")) continue;
				out[e["id"].get<std::string>()] = e["protocol_id"].get<std::int32_t>();
			}
		}
		catch (const std::exception& e) {
			LOG_WARN(Data, "parse failed: {} ({})", path, e.what());
		}

		LOG_DEBUG(Data, "enchantment_ids: {} entries", out.size());
	}

	// blocks/state_ids.json, one pass over the dump: the default state id and every
	// variant of the block sit in the same array entry.
	//   { data: [ { id, default_state_id, min_state_id, max_state_id, states: [...] } ] }
	inline void load_blocks(RegistryData& out, const std::string& path) {
		std::string text;
		if (!input::read_text(path, text)) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}
		simdjson::padded_string json{ std::string_view(text) };

		simdjson::dom::parser parser;
		simdjson::dom::element doc;
		if (parser.parse(json).get(doc) != simdjson::SUCCESS) {
			LOG_WARN(Data, "parse failed: {}", path);
			return;
		}

		simdjson::dom::array data;
		if (doc["data"].get_array().get(data) != simdjson::SUCCESS) {
			LOG_WARN(Data, "unexpected format in {} (no 'data' array)", path);
			return;
		}

		for (simdjson::dom::element entry : data) {
			std::string_view name;
			if (entry["id"].get_string().get(name) != simdjson::SUCCESS) continue;
			const std::string block{ name };

			std::int64_t state_id = -1;
			entry["default_state_id"].get_int64().get(state_id);
			if (state_id < 0) entry["min_state_id"].get_int64().get(state_id);
			if (state_id >= 0) {
				out.block_state_ids.try_emplace(block, static_cast<std::int32_t>(state_id));
			}

			simdjson::dom::array states;
			if (entry["states"].get_array().get(states) != simdjson::SUCCESS) continue;

			auto& variants = out.block_states[block];
			variants.reserve(states.size());
			for (simdjson::dom::element variant : states) {
				std::int64_t id = -1;
				if (variant["id"].get_int64().get(id) != simdjson::SUCCESS) continue;

				BlockStateInfo info;
				info.id = static_cast<std::int32_t>(id);
				simdjson::dom::object properties;
				if (variant["properties"].get_object().get(properties) == simdjson::SUCCESS) {
					for (auto property : properties) {
						std::string_view value;
						if (property.value.get_string().get(value) != simdjson::SUCCESS) continue;
						info.properties.emplace(std::string(property.key), std::string(value));
					}
				}
				variants.push_back(std::move(info));
			}
		}

		LOG_DEBUG(Data, "blocks: {} ids, {} variant tables",
			out.block_state_ids.size(), out.block_states.size());
	}

	// worldgen/biomes.json format:
	//   { data: [ { id: "minecraft:badlands", protocol_id: N, data: {...} } ] }
	inline void load_biome_ids(std::unordered_map<std::string, std::int32_t>& out, const std::string& path) {
		std::string text;
		if (!input::read_text(path, text)) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(text);

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
		std::string text;
		if (!input::read_text(path, text)) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(text);

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
		std::string text;
		if (!input::read_text(path, text)) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(text);

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
		std::string text;
		if (!input::read_text(path, text)) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(text);

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
		std::string text;
		if (!input::read_text(path, text)) {
			LOG_WARN(Data, "cannot open {}", path);
			return;
		}

		try {
			auto j = nlohmann::json::parse(text);
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
