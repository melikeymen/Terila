#pragma once

#include "nbt/nbt_text.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/string.hpp"

#include <cstdint>
#include <format>
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
        LOG_DEBUG(Protocol, "write_system_chat: msg='{}' size={}", message, message.size());

        // A message longer than a protocol string never becomes a packet; saying so here is
        // clearer than failing after the component has been built and hex-dumped.
        if (message.size() > 32767) {
            LOG_WARN(Protocol, "write_system_chat: message is {} bytes, the limit is 32767",
                message.size());
            return false;
        }

        std::vector<std::uint8_t> payload;
        auto component = terila::nbt::text_component_nbt(message);
        // Only build the dump when it is going to be printed: formatting every byte of
        // every message costs more than writing the packet.
        if (terila::log::is_enabled(terila::log::Level::Debug, terila::log::Scope::Protocol)) {
            std::string hex;
            hex.reserve(component.size() * 3);
            for (auto b : component) {
                hex += std::format("{:02X} ", b);
            }
            LOG_DEBUG(Protocol, "write_system_chat: component size={} hex={}", component.size(),
                hex);
        }
        payload.insert(payload.end(), component.begin(), component.end());
        payload.push_back(0);   // overlay: shown in chat, not above the hotbar

        if (!write_packet(output, packet_ids().play_system_chat, payload)) {
            LOG_WARN(Protocol, "write_system_chat: failed");
            return false;
        }
        return true;
    }

} // namespace terila::protocol
