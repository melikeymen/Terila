#include "network/connection.hpp"

#include "protocol/item.hpp"

#include <algorithm>

namespace terila::network {

    void Connection::send_inventory_slot(std::int32_t slot) {
        const std::int32_t index = slot - kFirstInventorySlot;
        if (index < 0 || index >= kInventorySlots) {
            return;
        }

        const auto& entry = inventory_[static_cast<std::size_t>(index)];
        std::vector<std::uint8_t> out;
        if (terila::protocol::write_container_set_slot(out, 0, 0,
                static_cast<std::int16_t>(slot), entry.item_id, entry.count,
                entry.components.get(), &component_ids_)) {
            send(out);
        }

        if (entry.count > 0) {
            creative_slots_[slot] = entry.item_id;
        }
        else {
            creative_slots_.erase(slot);
        }

        if (slot == 36 + carried_slot_) {
            refresh_held_item();
        }
    }

    void Connection::set_inventory_slot(std::int32_t slot, std::int32_t item_id,
        std::int32_t count) {
        const std::int32_t index = slot - kFirstInventorySlot;
        if (index < 0 || index >= kInventorySlots) {
            return;
        }
        inventory_[static_cast<std::size_t>(index)] = InvSlot{ item_id, count };
    }

    // Collected drops merge into a matching stack first, then take the first free slot.
    // The held item is refreshed when the touched slot is the selected hotbar one.
    bool Connection::add_to_inventory(std::int32_t item_id, std::int32_t count,
        const terila::item::ComponentsPtr& components) {
        if (item_id <= 0 || count <= 0) {
            return false;
        }

        std::int32_t remaining = count;

        // Vanilla fills the hotbar before the main inventory, and existing stacks
        // before opening a new slot.
        for (int pass = 0; pass < 2 && remaining > 0; ++pass) {
            for (int region = 0; region < 2 && remaining > 0; ++region) {
                const std::int32_t first = region == 0 ? kMainInventorySlots : 0;
                const std::int32_t last = region == 0 ? kInventorySlots : kMainInventorySlots;

                for (std::int32_t i = first; i < last && remaining > 0; ++i) {
                    auto& slot = inventory_[static_cast<std::size_t>(i)];

                    if (pass == 0) {
                        if (slot.item_id != item_id || slot.count >= kMaxStackSize ||
                            !terila::item::same_components(slot.components, components)) {
                            continue;
                        }
                        const std::int32_t added = std::min(kMaxStackSize - slot.count, remaining);
                        slot.count += added;
                        remaining -= added;
                    }
                    else {
                        if (slot.count > 0) {
                            continue;
                        }
                        const std::int32_t added = std::min(kMaxStackSize, remaining);
                        slot.item_id = item_id;
                        slot.count = added;
                        slot.components = components;
                        remaining -= added;
                    }

                    send_inventory_slot(kFirstInventorySlot + i);
                }
            }
        }

        if (remaining > 0) {
            LOG_DEBUG(Network, "{} has no room for {} x{}", username_, item_id, remaining);
            return false;
        }

        LOG_DEBUG(Network, "{} picked up {} x{}", username_, item_id, count);
        return true;
    }

} // namespace terila::network
