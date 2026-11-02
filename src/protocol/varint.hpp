#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include <bit>
#include <algorithm>

namespace terila::protocol {

//		Minecraft VarInt encoding: 32-bit signed integer, LEB128
//		Negative values ​​always occupy exactly 5 bytes of space. 

	inline void write_varint(std::vector<std::uint8_t>& output, std::int32_t value) {

		std::uint32_t unsigned_value = static_cast<std::uint32_t>(value);

		while (true) {
			std::uint8_t current_byte = static_cast<std::uint8_t>(unsigned_value & 0x7F);
			unsigned_value >>= 7;

			if (unsigned_value != 0) {
				current_byte |= 0x80;
			}

			output.push_back(current_byte);

			if (unsigned_value == 0) {
				break;
			}
		}

	}

	struct VarIntResult {
		std::int32_t  value;
		std::size_t   consumed;
	};

	inline std::optional<VarIntResult> read_varint(const std::uint8_t* data, std::size_t length) {

		if (data == nullptr || length == 0) {
			return std::nullopt;
		}

		std::uint32_t accumulator = 0;
		std::size_t shift = 0;

		for (std::size_t index = 0; index < length; ++index) {
			if (index >= 5) {
				return std::nullopt;
			}

			std::uint8_t current_byte = data[index];

			std::uint32_t value_bits = current_byte & 0x7F;

			accumulator |= value_bits << shift;

			if ((current_byte & 0x80) == 0) {
				return VarIntResult{
					static_cast<std::int32_t>(accumulator),
					index + 1
				};
			}

			shift += 7;
		}

		return std::nullopt;
	}

	inline std::size_t varint_size(std::int32_t value) {
		// Cast to unsigned: bit_width on negative int32_t is undefined
		std::uint32_t unsigned_value = static_cast<std::uint32_t>(value);
		int bits = std::bit_width(unsigned_value);
		// Integer ceiling of bits / 7 (each VarInt byte holds 7 data bits)
		std::size_t size = std::max<std::size_t>(1, (bits + 6) / 7);
		return size;
	}
}