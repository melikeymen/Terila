#pragma once

#include "log/logger.hpp"
#include "protocol/varint.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace terila::protocol {

    struct StringResult {
        std::string value;
        std::size_t consumed;
    };

    inline bool write_string(std::vector<std::uint8_t>& output, const std::string& value) {

        std::size_t byte_length = value.size();

        if (byte_length > 32767) {
            LOG_WARN(Protocol, "write_string: string too long ({} bytes, max 32767)", byte_length);
            return false;
        }

        output.reserve(output.size() + 5 + byte_length);

        write_varint(output, static_cast<std::int32_t>(byte_length));
        output.insert(output.end(), value.begin(), value.end());

        return true;
    }

    inline std::optional<StringResult> read_string(const std::uint8_t* data, std::size_t length) {

        if (data == nullptr || length == 0) {
            LOG_TRACE(Protocol, "read_string: null data or zero length");
            return std::nullopt;
        }

        auto varint = read_varint(data, length);

        if (!varint.has_value()) {
            LOG_TRACE(Protocol, "read_string: incomplete length varint ({} bytes)", length);
            return std::nullopt;
        }

        std::int32_t string_length = varint->value;
        std::size_t header_size = varint->consumed;

        if (string_length < 0) {
            LOG_WARN(Protocol, "read_string: negative length={}", string_length);
            return std::nullopt;
        }

        if (string_length > 32767) {
            LOG_WARN(Protocol, "read_string: length too large ({} > 32767)", string_length);
            return std::nullopt;
        }

        if (header_size + static_cast<std::size_t>(string_length) > length) {
            LOG_TRACE(Protocol, "read_string: incomplete body (have={}, need={})", length, header_size + static_cast<std::size_t>(string_length));
            return std::nullopt;
        }

        std::string result(reinterpret_cast<const char*>(data + header_size),
            static_cast<std::size_t>(string_length));

        // A NUL never belongs in a Minecraft string, and every reader downstream would stop
        // at it while the rest of the buffer looked like data.
        if (result.find('\0') != std::string::npos) {
            LOG_WARN(Protocol, "read_string: string contains a NUL byte");
            return std::nullopt;
        }

        return StringResult{ result, header_size + string_length };

    }
}