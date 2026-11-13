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

    std::int32_t Connection::base_state_at(std::int32_t y) const {
        const std::int32_t local = y - spec_.min_y;
        if (local == 0) return bedrock_state_;
        if (local > 0 && local <= spec_.dirt_layers) return dirt_state_;
        if (local == spec_.dirt_layers + 1) return grass_state_;
        return air_state_;
    }

    std::int32_t Connection::state_at(const terila::world::BlockPos& pos) const {
        return world_.get(pos, base_state_at(pos.y));
    }

    bool Connection::inside_world(std::int32_t y) const {
        return y >= spec_.min_y && y < spec_.min_y + spec_.section_count * 16;
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
            send_block_change(target, existing, sequence);
            return;
        }

        const auto it = item_block_state_.find(held_item_id_);
        if (it == item_block_state_.end()) {
            LOG_DEBUG(Network, "held item {} has no block form", held_item_id_);
            return;
        }

        world_.set(target, it->second);
        persist_block_change(target);
        send_block_change(target, it->second, sequence);
        LOG_DEBUG(Network, "{} placed block state {} at ({}, {}, {})",
            username_, it->second, target.x, target.y, target.z);
    }

    double Connection::drop_ground_y() const {
        return static_cast<double>(spec_.min_y + spec_.dirt_layers + 2);
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
        drops_->spawn(it->second, 1, pos.x + 0.5, pos.y + 0.6, pos.z + 0.5, drop_ground_y());
    }

    void Connection::give_hotbar_item(std::int32_t item_id) {
        const std::int32_t slot = 36 + carried_slot_;
        creative_slots_[slot] = item_id;
        held_item_id_ = item_id;

        std::vector<std::uint8_t> out;
        if (terila::protocol::write_container_set_slot(out, 0, 0,
                static_cast<std::int16_t>(slot), item_id, 1)) {
            send(out);
        }
        out.clear();
        if (terila::protocol::write_set_held_slot(out, carried_slot_)) {
            send(out);
        }
        LOG_DEBUG(Network, "{} picked item {} into hotbar slot {}",
            username_, item_id, carried_slot_);
    }

    void Connection::handle_pick_block(const terila::protocol::BlockPosValue& clicked) {
        const terila::world::BlockPos target{ clicked.x, clicked.y, clicked.z };
        if (!inside_world(target.y)) {
            return;
        }
        const auto it = block_state_item_.find(state_at(target));
        if (it == block_state_item_.end()) {
            LOG_DEBUG(Network, "{} picked a block without an item form at ({}, {}, {})",
                username_, target.x, target.y, target.z);
            return;
        }
        give_hotbar_item(it->second);
    }

    void Connection::refresh_held_item() {
        const std::int32_t hotbar_slot = 36 + carried_slot_;
        const auto it = creative_slots_.find(hotbar_slot);
        held_item_id_ = (it == creative_slots_.end()) ? 0 : it->second;
        LOG_DEBUG(Network, "held item is now {} (hotbar slot {})", held_item_id_, carried_slot_);
    }

} // namespace terila::network
