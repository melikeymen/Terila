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

}