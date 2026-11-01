#pragma once

#include "protocol/varint.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace terila::protocol {

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

}