#include "network/connection.hpp"

#include "protocol/block.hpp"
#include "protocol/item.hpp"
#include "protocol/play.hpp"

namespace terila::network {

    std::optional<std::int32_t> Connection::block_state(std::string_view name) const {
        const auto it = data_.block_state_ids.find(std::string(name));
        if (it == data_.block_state_ids.end()) return std::nullopt;
        return it->second;
    }

    std::optional<std::int32_t> Connection::biome_state(std::string_view name) const {
        const auto it = data_.biome_ids.find(std::string(name));
        if (it == data_.biome_ids.end()) return std::nullopt;
        return it->second;
    }

    std::int32_t Connection::state_at(const terila::world::BlockPos& pos) const {
        return world_.get(pos, terrain_.state_at(pos.x, pos.y, pos.z));
    }

    bool Connection::inside_world(std::int32_t y) const {
        return y >= terrain_.min_y() && y < terrain_.min_y() + terrain_.section_count() * 16;
    }

    void Connection::send_block_change(const terila::world::BlockPos& pos, std::int32_t state,
        std::int32_t sequence) {
        std::vector<std::uint8_t> out;
        if (terila::protocol::write_block_update(out, pos.x, pos.y, pos.z, state)) {
            send(out);
        }
        out.clear();
        if (terila::protocol::write_block_changed_ack(out, sequence)) {
            send(out);
        }
    }

    void Connection::handle_block_break(const terila::protocol::BlockPosValue& clicked,
        std::int32_t sequence) {
        if (!can_break_blocks()) {
            return;
        }
        if (menu_open_) {
            close_menu();
        }

        const terila::world::BlockPos target{ clicked.x, clicked.y, clicked.z };
        if (!inside_world(target.y)) {
            return;
        }
        if (state_at(target) == air_state_) {
            return;
        }

        const std::int32_t broken = state_at(target);
        world_.set(target, air_state_);
        spawn_block_drop(target, broken);
        persist_block_change(target);
        send_block_change(target, air_state_, sequence);
        LOG_DEBUG(Network, "{} broke a block at ({}, {}, {})",
            username_, target.x, target.y, target.z);
    }

    void Connection::handle_block_place(const terila::protocol::BlockPosValue& clicked,
        std::int32_t face, std::int32_t sequence) {
        static constexpr std::int32_t kFaceOffset[6][3] = {
            { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }
        };
        // A crafting table or furnace opens its window instead of being placed against.
        const terila::world::BlockPos clicked_pos{ clicked.x, clicked.y, clicked.z };
        if (const int menu = menu_kind_for_state(state_at(clicked_pos)); menu != kMenuNone) {
            open_menu(menu, menu == kMenuCrafting ? "Crafting" : "Furnace");
            return;
        }

        if (!can_place_blocks() || face < 0 || face > 5) {
            return;
        }

        const terila::world::BlockPos target{ clicked.x + kFaceOffset[face][0],
                                              clicked.y + kFaceOffset[face][1],
                                              clicked.z + kFaceOffset[face][2] };
        if (!inside_world(target.y)) {
            return;
        }
        const std::int32_t existing = state_at(target);
        if (existing != air_state_) {
            // The client predicted a placement the server does not agree with; send the
            // real state back so the ghost block disappears instead of lingering until
            // the chunk is streamed again.
            LOG_DEBUG(Network, "{}: placement refused, ({}, {}, {}) already holds state {}",
                username_, target.x, target.y, target.z, existing);
            send_block_change(target, existing, sequence);
            return;
        }

        const auto it = item_block_state_.find(held_item_id_);
        if (it == item_block_state_.end()) {
            // A refused placement leaves the client with a predicted block that only goes
            // away when the chunk is streamed again, so say why it was refused.
            LOG_INFO(Network, "{}: placement refused, held item {} has no block form (slot {})",
                username_, held_item_id_, carried_slot_);
            return;
        }

        std::int32_t placed = it->second;
        if (furnace_item_id_ != 0 && held_item_id_ == furnace_item_id_) {
            // A furnace faces whoever placed it: the front points back along their view.
            const int dir = ((static_cast<int>(std::floor(yaw_ / 90.0f + 0.5f)) % 4) + 4) % 4;
            static constexpr int kOppositeFacing[4] = { 0, 3, 1, 2 };
            const std::int32_t variant = furnace_states_[static_cast<std::size_t>(
                kOppositeFacing[dir] * 2)];
            if (variant >= 0) {
                placed = variant;
            }
        }

        world_.set(target, placed);
        persist_block_change(target);
        send_block_change(target, placed, sequence);
        LOG_DEBUG(Network, "{} placed block state {} at ({}, {}, {})",
            username_, placed, target.x, target.y, target.z);
    }

    // Drops fall onto the surface under the block that produced them, not onto the height
    // of the spawn column.
    double Connection::drop_ground_y(std::int32_t x, std::int32_t z) const {
        return static_cast<double>(terrain_.surface_height(x, z) + 2);
    }

    // Every broken block turns into a drop so the world feels physical; blocks without
    // an item form (like fire) simply vanish.
    void Connection::spawn_block_drop(const terila::world::BlockPos& pos, std::int32_t state) {
        if (drops_ == nullptr) {
            return;
        }
        const auto it = block_state_item_.find(state);
        if (it == block_state_item_.end()) {
            LOG_DEBUG(Network, "block state {} has no item form", state);
            return;
        }
        drops_->spawn(it->second, 1, pos.x + 0.5, pos.y + 0.6, pos.z + 0.5,
            drop_ground_y(pos.x, pos.z));
    }

    void Connection::give_hotbar_item(std::int32_t item_id) {
        const std::int32_t slot = 36 + carried_slot_;
        set_inventory_slot(slot, item_id, 1);
        send_inventory_slot(slot);

        std::vector<std::uint8_t> out;
        if (terila::protocol::write_set_held_slot(out, carried_slot_)) {
            send(out);
        }
        LOG_DEBUG(Network, "{} picked item {} into hotbar slot {}",
            username_, item_id, carried_slot_);
    }

    void Connection::handle_pick_block(const terila::protocol::BlockPosValue& clicked) {
        const terila::world::BlockPos target{ clicked.x, clicked.y, clicked.z };
        LOG_DEBUG(Network, "pick_block at ({}, {}, {})", target.x, target.y, target.z);

        if (!inside_world(target.y)) {
            LOG_DEBUG(Network, "pick_block: y={} outside world", target.y);
            return;
        }

        const std::int32_t state = state_at(target);
        LOG_DEBUG(Network, "pick_block: state={} held={}", state, held_item_id_);

        const auto it = block_state_item_.find(state);
        if (it == block_state_item_.end()) {
            LOG_DEBUG(Network, "pick_block: no item for state {} (map size={})",
                state, block_state_item_.size());
            return;
        }

        if (gamemode_ != 1) {
            // Survival and adventure only select a stack the player already owns;
            // creating the item would be free blocks.
            for (std::int32_t i = kMainInventorySlots; i < kInventorySlots; ++i) {
                const auto& slot = inventory_[static_cast<std::size_t>(i)];
                if (slot.item_id != it->second || slot.count <= 0) {
                    continue;
                }
                carried_slot_ = i - kMainInventorySlots;
                std::vector<std::uint8_t> out;
                if (terila::protocol::write_set_held_slot(out, carried_slot_)) {
                    send(out);
                }
                refresh_held_item();
                return;
            }
            return;
        }

        give_hotbar_item(it->second);
    }
    // The held item is whatever the selected hotbar slot holds. The inventory is the
    // authority in both game modes — creative only mirrors it — so a stack a player picked
    // up, crafted or was given is seen exactly like one taken from the creative menu.
    void Connection::refresh_held_item() {
        const std::int32_t hotbar_slot = 36 + carried_slot_;
        const std::int32_t index = hotbar_slot - kFirstInventorySlot;

        held_item_id_ = 0;
        if (index >= 0 && index < kInventorySlots) {
            const auto& slot = inventory_[static_cast<std::size_t>(index)];
            if (slot.count > 0) {
                held_item_id_ = slot.item_id;
            }
        }
        if (held_item_id_ == 0) {
            const auto it = creative_slots_.find(hotbar_slot);
            held_item_id_ = (it == creative_slots_.end()) ? 0 : it->second;
        }
        LOG_DEBUG(Network, "held item is now {} (hotbar slot {})", held_item_id_, carried_slot_);
    }

} // namespace terila::network
