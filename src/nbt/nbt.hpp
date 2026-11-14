#pragma once

#include "nbt/nbt_reader.hpp"
#include "nbt/nbt_writer.hpp"
#include "protocol/primitives.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace terila::nbt {

inline void write_name(std::vector<std::uint8_t>& out, std::string_view name) {
    terila::protocol::write_uint16(out, static_cast<std::uint16_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
}

inline void write_header(std::vector<std::uint8_t>& out, Tag tag, std::string_view name) {
    out.push_back(static_cast<std::uint8_t>(tag));
    write_name(out, name);
}

inline void begin_root(std::vector<std::uint8_t>& out) {
    out.push_back(static_cast<std::uint8_t>(Tag::Compound));
}

inline void begin_compound(std::vector<std::uint8_t>& out, std::string_view name) {
    write_header(out, Tag::Compound, name);
}

inline void end(std::vector<std::uint8_t>& out) { out.push_back(static_cast<std::uint8_t>(Tag::End)); }

inline void write_string(std::vector<std::uint8_t>& out, std::string_view name, std::string_view value) {
    write_header(out, Tag::String, name);
    write_name(out, value);
}

inline void write_int(std::vector<std::uint8_t>& out, std::string_view name, std::int32_t value) {
    write_header(out, Tag::Int, name);
    terila::protocol::write_int32(out, value);
}

inline void write_byte(std::vector<std::uint8_t>& out, std::string_view name, std::uint8_t value) {
    write_header(out, Tag::Byte, name);
    out.push_back(value);
}

inline void write_text_component(std::vector<std::uint8_t>& out, std::string_view text) {
    begin_root(out);
    write_string(out, "text", text);
    end(out);
}

}
