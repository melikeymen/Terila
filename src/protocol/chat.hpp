#pragma once

#include "nbt/nbt_writer.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/string.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terila::protocol {

    // A chat command arrives without its leading slash; the server picks the handler.
    inline std::optional<std::string> read_chat_command(const std::vector<std::uint8_t>& payload) {
        const auto command = read_string(payload.data(), payload.size());
        if (!command) {
            LOG_WARN(Protocol, "read_chat_command: malformed packet");
            return std::nullopt;
        }
        return command->value;
    }

    inline bool write_system_chat(std::vector<std::uint8_t>& output, std::string_view message) {
        std::vector<std::uint8_t> payload;
        terila::nbt::Writer writer(terila::nbt::StringFormat::Network);
        writer.begin_root_compound();
        writer.write_string("text", message);
        writer.end_compound();
        auto component = writer.finish();
        payload.insert(payload.end(), component.begin(), component.end());
        payload.push_back(0);   // overlay: shown in chat, not above the hotbar

        if (!write_packet(output, packet_ids().play_system_chat, payload)) {
            LOG_WARN(Protocol, "write_system_chat: failed");
            return false;
        }
        return true;
    }

} // namespace terila::protocol
