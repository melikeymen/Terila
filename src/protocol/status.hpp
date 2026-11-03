#pragma once

#include <vector>
#include "protocol/varint.hpp"
#include "protocol/string.hpp"
#include "protocol/packet.hpp"
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
        std::string           favicon;      // "data:image/png;base64,..." veya ""
    };

    inline void write_status_request(std::vector<std::uint8_t>& output) {
        write_packet(output, 0x00, {});
    }

    inline bool read_status_request(const std::uint8_t* data, std::size_t length) {
        auto result = read_packet(data, length);

        if (!result.has_value()) {
            return false;
        }

        if (result->id != 0x00) {
            return false;
        }

        if (!result->payload.empty()) {
            return false;
        }

        return true;
    }

    inline void write_ping(std::vector<std::uint8_t>& output, std::int64_t value) {
        std::vector<std::uint8_t> payload;

        write_int64(payload, value);
        write_packet(output, 0x01, payload);
    }

    inline std::optional<std::int64_t> read_ping(const std::uint8_t* data, std::size_t length) {
        auto result = read_packet(data, length);

        if (!result.has_value()) {
            return std::nullopt;
        }

        if (result->id != 0x01) {
            return std::nullopt;
        }

        return read_int64(result->payload.data(), result->payload.size());
    }

    inline void write_pong(std::vector<std::uint8_t>& output, std::int64_t value) {

        std::vector<std::uint8_t> payload;

        write_int64(payload, value);
        write_packet(output, 0x01, payload);
    }

    inline std::optional<std::int64_t> read_pong(const std::uint8_t* data, std::size_t length) {

        auto result = read_packet(data, length);
        
        if (!result.has_value()) {
            return std::nullopt;
        }

        if (result->id != 0x01) {
            return std::nullopt;
        }

        return read_int64(result->payload.data(), result->payload.size());
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
            return false;
        }

        return write_packet(output, 0x00, payload);
    }

    inline std::optional<StatusResponse> read_status_response(const std::uint8_t* data, std::size_t length) {

        auto packet = read_packet(data, length);

        if (!packet.has_value()) {
            return std::nullopt;
        }

        if (packet->id != 0x00) {
            return std::nullopt;
        }

        auto json_result = read_string(packet->payload.data(), packet->payload.size());
        if (!json_result.has_value()) {
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
        catch (const nlohmann::json::exception&) {
            return std::nullopt;
        }
    }
}