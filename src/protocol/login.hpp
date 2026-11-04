#pragma once 

#include "protocol/varint.hpp"
#include "protocol/packet.hpp"
#include "protocol/string.hpp"
#include <vector>
#include <optional>
#include <cstdint>
#include <string>
#include <array>

namespace terila::protocol {
	
	struct LoginStartResult {
		std::string username;
	};

	inline std::optional<LoginStartResult> read_login_start(const std::uint8_t* data, std::size_t length) {
		auto result = read_packet(data, length);
		if (!result.has_value()) {
			return std::nullopt;
		}

		if (result->id != 0x00) {
			return std::nullopt;
		}

		auto username_result = read_string(result->payload.data(), result->payload.size());

		if (!username_result.has_value()) {
			return std::nullopt;
		}

		return LoginStartResult { username_result->value };
	}

	inline bool write_login_success(std::vector<std::uint8_t>& output, const std::string& username, const std::array<std::uint8_t, 16>& uuid, const std::array<std::uint8_t, 16>& session_id) {

		std::vector<std::uint8_t> payload;


		payload.insert(payload.end(), uuid.begin(), uuid.end());

		if (!write_string(payload, username)) {
			return false;
		}

		write_varint(payload, 0);

		payload.insert(payload.end(), session_id.begin(), session_id.end());

		return write_packet(output, 0x02, payload);
	}
}