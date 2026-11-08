#pragma once

#include "log/logger.hpp"
#include <vector>
#include <cstdint>
#include <optional>
#include <cstddef>

namespace terila::protocol {

	inline void write_uint16(std::vector<std::uint8_t>& output, std::uint16_t value) {
		output.push_back(static_cast<uint8_t>(value >> 8));
		output.push_back(static_cast<uint8_t>(value & 0xFF));
	}

	inline std::optional<std::uint16_t> read_uint16(const std::uint8_t* data, std::size_t length) {
		if (data == nullptr || length < 2) {
			LOG_TRACE(Protocol, "read_uint16: insufficient data (length={})", length);
			return std::nullopt;
		}
		auto high = data[0];
		auto low = data[1];

		return static_cast<uint16_t>((high << 8) | low);
	}

	inline void write_int64(std::vector<std::uint8_t>& output, std::int64_t value) {
		std::uint64_t unsigned_value = static_cast<uint64_t>(value);
		for (int i = 7; i >= 0; --i) {
			output.push_back(static_cast<uint8_t>((unsigned_value >> (i * 8)) & 0xFF));
		}
	}

	inline std::optional<std::int64_t> read_int64(const std::uint8_t* data, std::size_t length) {
		if (data == nullptr || length < 8) {
			LOG_TRACE(Protocol, "read_int64: insufficient data (length={})", length);
			return std::nullopt;
		}
		uint64_t value = 0;

		for (int i = 0; i <= 7; i++) {
			value = (value << 8) | data[i];
		}

		return static_cast<int64_t>(value);
	}

}