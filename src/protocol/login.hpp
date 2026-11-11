#pragma once

#include "protocol/varint.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/string.hpp"
#include "log/logger.hpp"
#include <vector>
#include <optional>
#include <cstdint>
#include <string>
#include <array>
#include <cctype>

namespace terila::protocol {

	struct LoginStartResult {
		std::string username;
	};

	// 3-16 chars, only a-z, A-Z, 0-9 and '_'
	inline bool is_valid_username(const std::string& name) {
		if (name.size() < 3 || name.size() > 16) {
			return false;
		}
		for (char c : name) {
			unsigned char uc = static_cast<unsigned char>(c);
			if (!(std::isalnum(uc) || c == '_')) {
				return false;
			}
		}
		return true;
	}

	inline std::optional<LoginStartResult> read_login_start(const std::uint8_t* data, std::size_t length) {
		auto result = read_packet(data, length);
		if (!result.has_value()) {
			LOG_TRACE(Protocol, "read_login_start: incomplete packet ({} bytes)", length);
			return std::nullopt;
		}

		if (result->id != packet_ids().login_start) {
			LOG_WARN(Protocol, "read_login_start: unexpected packet id=0x{:02X} (expected 0x{:02X})", result->id, packet_ids().login_start);
			return std::nullopt;
		}

		auto username_result = read_string(result->payload.data(), result->payload.size());

		if (!username_result.has_value()) {
			LOG_WARN(Protocol, "read_login_start: failed to read username (payload={} bytes)",
				result->payload.size());
			return std::nullopt;
		}

		if (!is_valid_username(username_result->value)) {
			LOG_WARN(Protocol, "read_login_start: invalid username length={} (expected 3-16, a-z/A-Z/0-9/_)",
				username_result->value.size());
			return std::nullopt;
		}

		LOG_DEBUG(Protocol, "read_login_start: username={}", username_result->value);

		return LoginStartResult{ username_result->value };
	}

	inline bool write_login_success(std::vector<std::uint8_t>& output, const std::string& username, const std::array<std::uint8_t, 16>& uuid, const std::array<std::uint8_t, 16>& session_id) {

		if (!is_valid_username(username)) {
			LOG_WARN(Protocol, "write_login_success: invalid username length={} (expected 3-16, a-z/A-Z/0-9/_)",
				username.size());
			return false;
		}

		std::vector<std::uint8_t> payload;

		payload.insert(payload.end(), uuid.begin(), uuid.end());

		if (!write_string(payload, username)) {
			LOG_WARN(Protocol, "write_login_success: failed to write username ({} chars)", username.size());
			return false;
		}

		write_varint(payload, 0);

		payload.insert(payload.end(), session_id.begin(), session_id.end());

		if (!write_packet(output, packet_ids().login_success, payload)) {
			LOG_WARN(Protocol, "write_login_success: failed to write packet for username={}", username);
			return false;
		}

		LOG_DEBUG(Protocol, "write_login_success: username={} payload={} bytes",
			username, payload.size());

		return true;
	}
}