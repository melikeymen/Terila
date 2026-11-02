#pragma once 

#include "protocol/varint.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace terila::protocol {


	struct PacketResult {
		std::int32_t id;
		std::vector<std::uint8_t> payload;
		std::size_t consumed;
	};

	inline bool write_packet(std::vector<std::uint8_t>& output, std::int32_t id, const std::vector<std::uint8_t>& payload) {
		
		if (id < 0) {
			return false;
		}
		
		std::size_t packet_length = varint_size(id) + payload.size();

		// Max packet size: 2 MB (DoS protection)
		if (packet_length > 2 * 1024 * 1024) {
			return false;
		}

		write_varint(output, static_cast<std::int32_t>(packet_length));
		write_varint(output, id);
		output.insert(output.end(), payload.begin(), payload.end());
		return true;


	}
}