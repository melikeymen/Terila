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

	inline std::optional<PacketResult> read_packet(const std::uint8_t* data, std::size_t length) {
		if (data == nullptr || length == 0) {
			return std::nullopt;
		}

		auto packet_length_result = read_varint(data, length);
		if (!packet_length_result.has_value() || packet_length_result->value > 2 * 1024 * 1024 || packet_length_result->value < 0) {
			return std::nullopt;
		}

		if (packet_length_result->consumed + static_cast<std::size_t>(packet_length_result->value) > length) {
			return std::nullopt;
		}

		auto id_result = read_varint(data + packet_length_result->consumed, static_cast<std::size_t>(packet_length_result->value));

		if (!id_result.has_value() || id_result->value < 0) {
			return std::nullopt;
		}

		if (id_result->consumed > packet_length_result->value) {
			return std::nullopt;
		}

		auto payload_size = packet_length_result->value - id_result->consumed;

		std::vector<std::uint8_t> payload(
			data + packet_length_result->consumed + id_result->consumed,
			data + packet_length_result->consumed + id_result->consumed + payload_size
		);

		auto consumed = packet_length_result->consumed + packet_length_result->value;

		return PacketResult{ id_result->value, payload, consumed };
	}

}