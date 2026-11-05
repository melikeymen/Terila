#pragma once

#include "data/registry_data.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>

namespace terila::data {

	inline bool load_binary(std::vector<std::uint8_t>& out, const std::string& path) {
		std::ifstream file(path, std::ios::binary);

		if (!file) {
			return false;
		}

		out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

		return !out.empty();
	}

	inline bool load_command_tree(std::vector<std::uint8_t>& out, const std::string& path) {

		if (!load_binary(out, path)) {
			return false;
		}

		std::size_t i = 0;

		while (i < out.size() && (out[i] & 0x80)) ++i;

		if (i < out.size()) ++i;

		out.erase(out.begin(), out.begin() + i);

		return !out.empty();
	}

}