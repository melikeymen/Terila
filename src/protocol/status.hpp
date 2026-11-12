#pragma once

#include "log/logger.hpp"
#include <vector>
#include "protocol/varint.hpp"
#include "protocol/string.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/primitives.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <cstddef>

namespace terila::protocol {

    struct StatusResponse {
        std::string      version_name;      // "26.3"
        std::int32_t version_protocol;      // 777
        std::int32_t      players_max;      // 20
        std::int32_t   players_online;      // 0
        std::string       description;      // "Terila Server"
        std::string           favicon;      // "data:image/png;base64,..." or ""
    };

    inline void write_status_request(std::vector<std::uint8_t>& output) {
        write_packet(output, packet_ids().status_request, {});
    }

    inline bool read_status_request(const std::uint8_t* data, std::size_t length) {
        auto result = read_packet(data, length);

        if (!result.has_value()) {
            return false;
        }

        if (result->id != packet_ids().status_request) {
            LOG_WARN(Protocol, "read_status_request: unexpected packet id=0x{:02X} (expected 0x{:02X})", result->id, packet_ids().status_request);
            return false;
        }

        if (!result->payload.empty()) {
            LOG_WARN(Protocol, "read_status_request: expected empty payload, got {} bytes", result->payload.size());
            return false;
        }

        return true;
    }

    inline void write_ping(std::vector<std::uint8_t>& output, std::int64_t value) {
        std::vector<std::uint8_t> payload;

        write_int64(payload, value);
        write_packet(output, packet_ids().ping, payload);

        LOG_TRACE(Protocol, "write_ping: value={}", value);
    }

    inline std::optional<std::int64_t> read_ping(const std::uint8_t* data, std::size_t length) {
        auto result = read_packet(data, length);

        if (!result.has_value()) {
            return std::nullopt;
        }

        if (result->id != packet_ids().ping) {
            LOG_WARN(Protocol, "read_ping: unexpected packet id=0x{:02X} (expected 0x{:02X})", result->id, packet_ids().ping);
            return std::nullopt;
        }

        auto value = read_int64(result->payload.data(), result->payload.size());
        if (!value.has_value()) {
            LOG_WARN(Protocol, "read_ping: failed to read int64 (payload={} bytes)", result->payload.size());
            return std::nullopt;
        }

        LOG_TRACE(Protocol, "read_ping: value={}", *value);
        return value;
    }

    inline void write_pong(std::vector<std::uint8_t>& output, std::int64_t value) {
        std::vector<std::uint8_t> payload;

        write_int64(payload, value);
        write_packet(output, packet_ids().status_pong, payload);

        LOG_TRACE(Protocol, "write_pong: value={}", value);
    }

    inline std::optional<std::int64_t> read_pong(const std::uint8_t* data, std::size_t length) {
        auto result = read_packet(data, length);

        if (!result.has_value()) {
            return std::nullopt;
        }

        if (result->id != packet_ids().ping) {
            LOG_WARN(Protocol, "read_pong: unexpected packet id=0x{:02X} (expected 0x{:02X})", result->id, packet_ids().ping);
            return std::nullopt;
        }

        auto value = read_int64(result->payload.data(), result->payload.size());
        if (!value.has_value()) {
            LOG_WARN(Protocol, "read_pong: failed to read int64 (payload={} bytes)", result->payload.size());
            return std::nullopt;
        }

        return value;
    }

    inline bool write_status_response(std::vector<std::uint8_t>& output, const StatusResponse& response) {
        nlohmann::json j;

        j["version"]["name"] = response.version_name;
        j["version"]["protocol"] = response.version_protocol;
        j["players"]["max"] = response.players_max;
        j["players"]["online"] = response.players_online;
        j["description"]["text"] = response.description;

        if (!response.favicon.empty()) {
            j["favicon"] = response.favicon;
        }

        std::string json_str = j.dump();

        std::vector<std::uint8_t> payload;
        if (!write_string(payload, json_str)) {
            LOG_WARN(Protocol, "write_status_response: failed to write json string ({} bytes)", json_str.size());
            return false;
        }

        if (!write_packet(output, packet_ids().status_response, payload)) {
            LOG_WARN(Protocol, "write_status_response: failed to write packet");
            return false;
        }

        LOG_DEBUG(Protocol, "write_status_response: {} players online, json={} bytes",
            response.players_online, json_str.size());
        return true;
    }

    inline std::optional<StatusResponse> read_status_response(const std::uint8_t* data, std::size_t length) {

        auto packet = read_packet(data, length);

        if (!packet.has_value()) {
            return std::nullopt;
        }

        if (packet->id != packet_ids().status_response) {
            LOG_WARN(Protocol, "read_status_response: unexpected packet id=0x{:02X} (expected 0x{:02X})", packet->id, packet_ids().status_response);
            return std::nullopt;
        }

        auto json_result = read_string(packet->payload.data(), packet->payload.size());
        if (!json_result.has_value()) {
            LOG_WARN(Protocol, "read_status_response: failed to read json string (payload={} bytes)",
                packet->payload.size());
            return std::nullopt;
        }

        try {
            auto j = nlohmann::json::parse(json_result->value);

            StatusResponse response;
            response.version_name = j["version"]["name"].get<std::string>();
            response.version_protocol = j["version"]["protocol"].get<std::int32_t>();
            response.players_max = j["players"]["max"].get<std::int32_t>();
            response.players_online = j["players"]["online"].get<std::int32_t>();
            response.description = j["description"]["text"].get<std::string>();

            if (j.contains("favicon")) {
                response.favicon = j["favicon"].get<std::string>();
            }
            else {
                response.favicon = "";
            }

            return response;
        }
        catch (const nlohmann::json::exception& e) {
            LOG_WARN(Protocol, "read_status_response: json parse failed: {}", e.what());
            return std::nullopt;
        }
    }
}