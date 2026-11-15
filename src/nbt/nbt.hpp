#pragma once

#include "nbt/nbt_reader.hpp"
#include "nbt/nbt_writer.hpp"
#include "nbt/snbt.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace terila::nbt {

inline void write_text_component(std::vector<std::uint8_t>& out, std::string_view text) {
    Writer writer(StringFormat::Network);
    writer.begin_root_compound();
    writer.write_string("text", text);
    writer.end_compound();
    auto bytes = writer.finish();
    out.insert(out.end(), bytes.begin(), bytes.end());
}

}
