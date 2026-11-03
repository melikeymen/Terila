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
}