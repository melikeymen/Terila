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
}