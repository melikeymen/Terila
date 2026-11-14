#pragma once

#include "item/components.hpp"
#include "log/logger.hpp"
#include "protocol/item.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/primitives.hpp"
#include "protocol/varint.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace terila::protocol {

    struct MenuItem {
        std::int32_t item_id = 0;
        std::int32_t count = 0;
        const terila::item::Components* components = nullptr;
    };

    // Network NBT text component: an unnamed compound that only carries "text".
    inline void write_component_text(std::vector<std::uint8_t>& output, std::string_view text) {
        output.push_back(0x0A);   // TAG_Compound, no root name on the wire
        output.push_back(0x08);   // TAG_String
        write_uint16(output, 4);
        output.insert(output.end(), { 't', 'e', 'x', 't' });
        write_uint16(output, static_cast<std::uint16_t>(text.size()));
        output.insert(output.end(), text.begin(), text.end());
        output.push_back(0x00);   // TAG_End
    }

    inline bool write_open_screen(std::vector<std::uint8_t>& output, std::int32_t window_id,
        std::int32_t menu_type, std::string_view title) {
        std::vector<std::uint8_t> payload;
        write_varint(payload, window_id);
        write_varint(payload, menu_type);
        write_component_text(payload, title);

        if (!write_packet(output, packet_ids().play_open_screen, payload)) {
            LOG_WARN(Protocol, "write_open_screen: failed (window={})", window_id);
            return false;
        }
        return true;
    }

    inline bool write_container_set_content(std::vector<std::uint8_t>& output,
        std::int32_t window_id, std::int32_t state_id,
        const std::vector<MenuItem>& items, MenuItem carried,
        const terila::item::ComponentIds* component_ids = nullptr) {
        std::vector<std::uint8_t> payload;
        write_varint(payload, window_id);
        write_varint(payload, state_id);
        write_varint(payload, static_cast<std::int32_t>(items.size()));
        for (const auto& item : items) {
            write_item_stack(payload, item.item_id, item.count, item.components, component_ids);
        }
        write_item_stack(payload, carried.item_id, carried.count, carried.components,
            component_ids);

        if (!write_packet(output, packet_ids().play_container_set_content, payload)) {
            LOG_WARN(Protocol, "write_container_set_content: failed (window={})", window_id);
            return false;
        }
        return true;
    }

    // Furnace progress/flame values: the client drives its arrow and flame from these.
    inline bool write_container_set_data(std::vector<std::uint8_t>& output,
        std::int32_t window_id, std::int32_t data_id, std::int32_t value) {
        std::vector<std::uint8_t> payload;
        write_varint(payload, window_id);
        write_uint16(payload, static_cast<std::uint16_t>(data_id));
        write_uint16(payload, static_cast<std::uint16_t>(value));

        if (!write_packet(output, packet_ids().play_container_set_data, payload)) {
            LOG_WARN(Protocol, "write_container_set_data: failed (window={}, id={})",
                window_id, data_id);
            return false;
        }
        return true;
    }

    struct ContainerClick {
        std::int32_t window_id = 0;
        std::int32_t state_id = 0;
        std::int32_t slot = 0;
        std::int32_t button = 0;
        std::int32_t click_type = 0;
    };

    // Only the fields the server acts on; the changed-slot map and the carried stack the
    // client echoes back are ignored because the server stays authoritative.
    inline std::optional<ContainerClick> read_container_click(const std::uint8_t* data,
        std::size_t length) {
        std::size_t offset = 0;

        const auto window = read_varint(data + offset, length - offset);
        if (!window) {
            return std::nullopt;
        }
        offset += window->consumed;

        const auto state = read_varint(data + offset, length - offset);
        if (!state) {
            return std::nullopt;
        }
        offset += state->consumed;

        if (length < offset + 2) {
            return std::nullopt;
        }
        const auto slot = static_cast<std::int16_t>((data[offset] << 8) | data[offset + 1]);
        offset += 2;

        if (length < offset + 1) {
            return std::nullopt;
        }
        const std::int32_t button = data[offset++];

        const auto click = read_varint(data + offset, length - offset);
        if (!click) {
            return std::nullopt;
        }

        ContainerClick result;
        result.window_id = window->value;
        result.state_id = state->value;
        result.slot = slot;
        result.button = button;
        result.click_type = click->value;
        return result;
    }

} // namespace terila::protocol
