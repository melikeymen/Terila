#pragma once

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
            return false;
        }

        output.reserve(output.size() + 5 + byte_length);

        write_varint(output, static_cast<std::int32_t>(byte_length));
        output.insert(output.end(), value.begin(), value.end());

        return true;
    }

    inline std::optional<StringResult> read_string(const std::uint8_t* data, std::size_t length) {

        if (data == nullptr || length == 0) {
            return std::nullopt;
        }

        auto varint = read_varint(data, length);

        if (!varint.has_value()) {
            return std::nullopt;
        }

        std::int32_t string_length = varint->value;
        std::size_t header_size = varint->consumed;

        if (string_length < 0) {
            return std::nullopt;
        }

        if (string_length > 32767) {
            return std::nullopt;
        }

        if (header_size + static_cast<std::size_t>(string_length) > length) {
            return std::nullopt;
        }

        std::string result(reinterpret_cast<const char*>(data + header_size),
        static_cast<std::size_t>(string_length));

        return StringResult{ result, header_size + string_length };

    }
}