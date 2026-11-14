#pragma once

#include "item/components.hpp"
#include "log/logger.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/primitives.hpp"
#include "protocol/varint.hpp"

#include <cstdint>
#include <vector>

namespace terila::protocol {

    // Metadata index of the stack an item entity renders and type 7 is an ItemStack;
    // both come from the vanilla entity data layout.
    inline constexpr std::uint8_t kItemEntityStackIndex = 8;
    inline constexpr std::int32_t kMetadataItemStack = 7;

    // Slot encoding since 1.20.5: the count first, then the item id with the component
    // patch. An empty patch means "take the components the item registry defines",
    // while a patch replaces defaults such as an empty enchantment list.
    inline void write_item_stack(std::vector<std::uint8_t>& output, std::int32_t item_id,
        std::int32_t count, const terila::item::Components* components = nullptr,
        const terila::item::ComponentIds* component_ids = nullptr) {
        write_varint(output, count);
        if (count <= 0) {
            return;
        }
        write_varint(output, item_id);

        const terila::item::ComponentIds ids =
            component_ids != nullptr ? *component_ids : terila::item::ComponentIds{};
        terila::item::write_component_patch(output, components, ids);
    }

    inline bool write_container_set_slot(std::vector<std::uint8_t>& output,
        std::uint8_t window_id, std::int32_t state_id, std::int16_t slot,
        std::int32_t item_id, std::int32_t count,
        const terila::item::Components* components = nullptr,
        const terila::item::ComponentIds* component_ids = nullptr) {
        std::vector<std::uint8_t> payload;
        payload.push_back(window_id);
        write_varint(payload, state_id);
        write_int16(payload, slot);
        write_item_stack(payload, item_id, count, components, component_ids);

        if (!write_packet(output, packet_ids().play_container_set_slot, payload)) {
            LOG_WARN(Protocol, "write_container_set_slot: failed (slot={})", slot);
            return false;
        }
        return true;
    }

    // A dropped item only renders once its stack is set, so add_entity is always
    // followed by this packet.
    inline bool write_item_entity_data(std::vector<std::uint8_t>& output, std::int32_t entity_id,
        std::int32_t item_id, std::int32_t count,
        const terila::item::Components* components = nullptr,
        const terila::item::ComponentIds* component_ids = nullptr) {
        std::vector<std::uint8_t> payload;
        write_varint(payload, entity_id);
        payload.push_back(kItemEntityStackIndex);
        write_varint(payload, kMetadataItemStack);
        write_item_stack(payload, item_id, count, components, component_ids);
        payload.push_back(0xFF);   // end of metadata

        if (!write_packet(output, packet_ids().play_set_entity_data, payload)) {
            LOG_WARN(Protocol, "write_item_entity_data: failed (id={})", entity_id);
            return false;
        }
        return true;
    }

    inline bool write_set_held_slot(std::vector<std::uint8_t>& output, std::int32_t slot) {
        std::vector<std::uint8_t> payload;
        payload.push_back(static_cast<std::uint8_t>(slot));

        if (!write_packet(output, packet_ids().play_set_held_slot, payload)) {
            LOG_WARN(Protocol, "write_set_held_slot: failed (slot={})", slot);
            return false;
        }
        return true;
    }

    // Makes the client animate the drop flying into the collector. Field order is
    // item entity first, then the collector, then the amount; the client casts the
    // second id to a LivingEntity.
    inline bool write_take_item_entity(std::vector<std::uint8_t>& output,
        std::int32_t item_entity_id, std::int32_t collector_id, std::int32_t amount) {
        std::vector<std::uint8_t> payload;
        write_varint(payload, item_entity_id);
        write_varint(payload, collector_id);
        write_varint(payload, amount);

        if (!write_packet(output, packet_ids().play_take_item_entity, payload)) {
            LOG_WARN(Protocol, "write_take_item_entity: failed (id={})", item_entity_id);
            return false;
        }
        return true;
    }

} // namespace terila::protocol
