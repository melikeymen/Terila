#pragma once

#include "protocol/varint.hpp"
#include "protocol/string.hpp"
#include "protocol/packet.hpp"
#include "log/logger.hpp"
#include <vector>
#include <optional>
#include <cstdint>
#include <string>

namespace terila::protocol {

	struct HandshakeResult {
		std::int32_t     protocol;
		std::string	      address;
		std::uint16_t        port;
		std::int32_t   next_state;
	};

	inline bool write_handshake(std::vector<std::uint8_t>& output, std::int32_t protocol, const std::string& address, std::uint16_t port, std::int32_t next_state) {
		std::vector<std::uint8_t> payload;

		terila::protocol::write_varint(payload, protocol);
		terila::protocol::write_string(payload, address);
		payload.push_back(static_cast<std::uint8_t>(port >> 8));
		payload.push_back(static_cast<std::uint8_t>(port & 0xFF));
		terila::protocol::write_varint(payload, next_state);

		LOG_DEBUG(Protocol, "write_handshake: protocol={} address={} port={} next_state={}", protocol, address, port, next_state);

		return write_packet(output, 0x00, payload);
	}

	inline std::optional<HandshakeResult> read_handshake(const std::uint8_t* data, std::size_t length) {

		auto packet = terila::protocol::read_packet(data, length);
		if (!packet.has_value()) {
			LOG_TRACE(Protocol, "read_handshake: incomplete packet ({} bytes)", length);
			return std::nullopt;
		}

		if (packet->id != 0x00) {
			LOG_WARN(Protocol, "read_handshake: unexpected packet id=0x{:02X} (expected 0x00)", packet->id);
			return std::nullopt;
		}

		auto protocol = terila::protocol::read_varint(packet->payload.data(), packet->payload.size());

		if (!protocol.has_value()) {
			LOG_WARN(Protocol, "read_handshake: failed to read protocol version");
			return std::nullopt;
		}

		auto address = terila::protocol::read_string(packet->payload.data() + protocol->consumed, packet->payload.size() - protocol->consumed);

		if (!address.has_value()) {
			LOG_WARN(Protocol, "read_handshake: failed to read address");
			return std::nullopt;
		}

		if (protocol->consumed + address->consumed + 2 > packet->payload.size()) {
			LOG_WARN(Protocol, "read_handshake: payload too short for port (size={}, need={})",
				packet->payload.size(), protocol->consumed + address->consumed + 2);
			return std::nullopt;
		}

		std::uint8_t high_port = packet->payload[protocol->consumed + address->consumed];
		std::uint8_t low_port = packet->payload[protocol->consumed + address->consumed + 1];

		std::uint16_t port = static_cast<std::uint16_t>((high_port << 8) | low_port);

		auto next_state = terila::protocol::read_varint(packet->payload.data() + protocol->consumed + address->consumed + 2, packet->payload.size() - protocol->consumed - address->consumed - 2);

		if (!next_state.has_value()) {
			LOG_WARN(Protocol, "read_handshake: failed to read next_state");
			return std::nullopt;
		}

		LOG_DEBUG(Protocol, "read_handshake: protocol={} address={} port={} next_state={}",
			protocol->value, address->value, port, next_state->value);

		return HandshakeResult{ protocol->value, address->value, port, next_state->value };
	}

}