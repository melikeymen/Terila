#pragma once

#include "log/logger.hpp"
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
			LOG_WARN(Protocol, "write_packet: unresolved packet id={} (missing ids.json entry)", id);
			return false;
		}

		std::size_t packet_length = varint_size(id) + payload.size();

		if (packet_length > 2 * 1024 * 1024) {
			LOG_WARN(Protocol, "write_packet: packet too large id=0x{:02X} size={} (max 2MB)", id, packet_length);
			return false;
		}

		write_varint(output, static_cast<std::int32_t>(packet_length));
		write_varint(output, id);
		output.insert(output.end(), payload.begin(), payload.end());

		LOG_TRACE(Protocol, "write_packet: id=0x{:02X} payload={} bytes", id, payload.size());
		return true;
	}

	inline std::optional<PacketResult> read_packet(const std::uint8_t* data, std::size_t length) {
		if (data == nullptr || length == 0) {
			LOG_TRACE(Protocol, "read_packet: null data or zero length");
			return std::nullopt;
		}

		auto packet_length_result = read_varint(data, length);
		if (!packet_length_result.has_value()) {
			LOG_TRACE(Protocol, "read_packet: incomplete length varint ({} bytes)", length);
			return std::nullopt;
		}

		if (packet_length_result->value > 2 * 1024 * 1024) {
			LOG_WARN(Protocol, "read_packet: packet too large length={}", packet_length_result->value);
			return std::nullopt;
		}

		if (packet_length_result->value < 0) {
			LOG_WARN(Protocol, "read_packet: negative packet length={}", packet_length_result->value);
			return std::nullopt;
		}

		if (packet_length_result->consumed + static_cast<std::size_t>(packet_length_result->value) > length) {
			LOG_TRACE(Protocol, "read_packet: incomplete body (have={}, need={})",
				length, packet_length_result->consumed + packet_length_result->value);
			return std::nullopt;
		}

		auto id_result = read_varint(data + packet_length_result->consumed, static_cast<std::size_t>(packet_length_result->value));

		if (!id_result.has_value()) {
			LOG_WARN(Protocol, "read_packet: failed to read id (length={})", packet_length_result->value);
			return std::nullopt;
		}

		if (id_result->value < 0) {
			LOG_WARN(Protocol, "read_packet: negative id={}", id_result->value);
			return std::nullopt;
		}

		if (id_result->consumed > packet_length_result->value) {
			LOG_WARN(Protocol, "read_packet: id varint overflows packet (id_consumed={}, packet_len={})",
				id_result->consumed, packet_length_result->value);
			return std::nullopt;
		}

		auto payload_size = packet_length_result->value - id_result->consumed;

		std::vector<std::uint8_t> payload(
			data + packet_length_result->consumed + id_result->consumed,
			data + packet_length_result->consumed + id_result->consumed + payload_size
		);

		auto consumed = packet_length_result->consumed + packet_length_result->value;

		LOG_TRACE(Protocol, "read_packet: id=0x{:02X} payload={} bytes consumed={}", id_result->value, payload_size, consumed);
		return PacketResult{ id_result->value, payload, consumed };
	}

}