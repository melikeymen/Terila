#pragma once

#include "nbt/nbt_writer.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace terila::nbt {

    // Classic NBT text component, used by clientbound chat and menu title
    // packets. The client decodes their Component field with
    // FriendlyByteBuf.readNbt(), which expects classic strings (uint16 length)
    // and a named root compound. Network NBT (VarInt strings, unnamed root)
    // here makes the client read VarInt(4) + 't' as a uint16 length of 1140
    // and disconnect on EOF.
    inline void write_text_component(std::vector<std::uint8_t>& out, std::string_view text) {
        Writer writer(StringFormat::Classic);
        writer.begin_root_compound();
        writer.write_string("text", text);
        writer.end_compound();
        auto bytes = writer.finish();
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

} // namespace terila::nbt