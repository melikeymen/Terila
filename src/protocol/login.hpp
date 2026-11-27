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
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>

namespace terila::protocol {

	struct LoginStartResult {
		std::string username;

		// The profile the client claims. A premium launcher puts the account's real UUID in
		// the hello packet and an offline one the UUID derived from the name — the same one
		// the server would compute. It is a claim, not a proof: the trust the name already
		// has, nothing more.
		std::array<std::uint8_t, 16> uuid{};
		bool has_uuid = false;
	};

	// 3-16 chars, only a-z, A-Z, 0-9 and '_'
	inline bool is_valid_username(const std::string& name) {
		if (name.size() < 3 || name.size() > 16) {
			return false;
		}
		for (char c : name) {
			// Explicit ranges: std::isalnum would depend on the process locale.
			const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			const bool digit = (c >= '0' && c <= '9');
			if (!(alpha || digit || c == '_')) {
				return false;
			}
		}
		return true;
	}

	// A complete packet that carries a name the server will not take is a refusal, not a
	// packet still in flight: the caller has to answer it instead of waiting for more bytes,
	// and `rejected` is how it tells the two apart.
	inline std::optional<LoginStartResult> read_login_start(const std::uint8_t* data, std::size_t length,
		bool* rejected = nullptr) {
		if (rejected != nullptr) {
			*rejected = false;
		}

		auto result = read_packet(data, length);
		if (!result.has_value()) {
			LOG_TRACE(Protocol, "read_login_start: incomplete packet ({} bytes)", length);
			return std::nullopt;
		}

		if (result->id != packet_ids().login_start) {
			LOG_WARN(Protocol, "read_login_start: unexpected packet id=0x{:02X} (expected 0x{:02X})", result->id, packet_ids().login_start);
			if (rejected != nullptr) {
				*rejected = true;
			}
			return std::nullopt;
		}

		auto username_result = read_string(result->payload.data(), result->payload.size());

		if (!username_result.has_value()) {
			LOG_WARN(Protocol, "read_login_start: failed to read username (payload={} bytes)",
				result->payload.size());
			if (rejected != nullptr) {
				*rejected = true;
			}
			return std::nullopt;
		}

		if (!is_valid_username(username_result->value)) {
			LOG_WARN(Protocol, "read_login_start: invalid username length={} (expected 3-16, a-z/A-Z/0-9/_)",
				username_result->value.size());
			if (rejected != nullptr) {
				*rejected = true;
			}
			return std::nullopt;
		}

		LOG_DEBUG(Protocol, "read_login_start: username={}", username_result->value);

		LoginStartResult start{ username_result->value };
		const std::size_t offset = username_result->consumed;
		if (result->payload.size() >= offset + 16) {
			std::copy_n(result->payload.begin() + static_cast<std::ptrdiff_t>(offset), 16,
				start.uuid.begin());
			start.has_uuid = true;
		}
		else {
			LOG_DEBUG(Protocol, "read_login_start: no profile UUID in the packet ({} bytes)",
				result->payload.size());
		}
		return start;
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