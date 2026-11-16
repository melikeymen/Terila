#pragma once

#include "log/logger.hpp"
#include <vector>
#include <cstdint>
#include <cstring>
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
		return static_cast<uint16_t>((data[0] << 8) | data[1]);
	}

	inline void write_int16(std::vector<std::uint8_t>& output, std::int16_t value) {
		const auto u = static_cast<std::uint16_t>(value);
		output.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
		output.push_back(static_cast<uint8_t>(u & 0xFF));
	}

	inline void write_int32(std::vector<std::uint8_t>& output, std::int32_t value) {
		std::uint32_t u = static_cast<std::uint32_t>(value);
		output.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
		output.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
		output.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
		output.push_back(static_cast<uint8_t>(u & 0xFF));
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
		for (int i = 0; i <= 7; i++) value = (value << 8) | data[i];
		return static_cast<int64_t>(value);
	}

	inline void write_float(std::vector<std::uint8_t>& output, float value) {
		std::uint32_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		output.push_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
		output.push_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
		output.push_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
		output.push_back(static_cast<uint8_t>(bits & 0xFF));
	}

	inline std::optional<double> read_double(const std::uint8_t* data, std::size_t length, std::size_t offset = 0) {
		// Written as a subtraction: offset + 8 wraps for an offset near the end of the address
		// space and the check would then pass.
		if (data == nullptr || offset > length || length - offset < 8) {
			LOG_TRACE(Protocol, "read_double: insufficient data (length={}, offset={})", length, offset);
			return std::nullopt;
		}
		std::uint64_t bits = 0;
		for (int i = 0; i < 8; ++i) bits = (bits << 8) | data[offset + i];
		double value = 0.0;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}

	inline void write_double(std::vector<std::uint8_t>& output, double value) {
		std::uint64_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		for (int i = 7; i >= 0; --i)
			output.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
	}

}