#pragma once

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
    // deltas. Empty deltas mean "take the components the item registry defines".
    inline void write_item_stack(std::vector<std::uint8_t>& output, std::int32_t item_id,
        std::int32_t count) {
        write_varint(output, count);
        if (count <= 0) {
            return;
        }
        write_varint(output, item_id);
        write_varint(output, 0);   // components added on top of the defaults
        write_varint(output, 0);   // components removed from the defaults
    }

    inline bool write_container_set_slot(std::vector<std::uint8_t>& output,
        std::uint8_t window_id, std::int32_t state_id, std::int16_t slot,
        std::int32_t item_id, std::int32_t count) {
        std::vector<std::uint8_t> payload;
        payload.push_back(window_id);
        write_varint(payload, state_id);
        write_int16(payload, slot);
        write_item_stack(payload, item_id, count);

        if (!write_packet(output, packet_ids().play_container_set_slot, payload)) {
            LOG_WARN(Protocol, "write_container_set_slot: failed (slot={})", slot);
            return false;
        }
        return true;
    }

    // A dropped item only renders once its stack is set, so add_entity is always
    // followed by this packet.
    inline bool write_item_entity_data(std::vector<std::uint8_t>& output, std::int32_t entity_id,
        std::int32_t item_id, std::int32_t count) {
        std::vector<std::uint8_t> payload;
        write_varint(payload, entity_id);
        payload.push_back(kItemEntityStackIndex);
        write_varint(payload, kMetadataItemStack);
        write_item_stack(payload, item_id, count);
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
