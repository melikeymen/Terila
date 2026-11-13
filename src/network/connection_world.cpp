#include "network/connection.hpp"

#include "protocol/block.hpp"
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
        const terila::world::BlockPos target{ clicked.x, clicked.y, clicked.z };
        if (!inside_world(target.y)) {
            return;
        }
        if (state_at(target) == air_state_) {
            return;
        }

        world_.set(target, air_state_);
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
        if (face < 0 || face > 5) {
            return;
        }

        const terila::world::BlockPos target{ clicked.x + kFaceOffset[face][0],
                                              clicked.y + kFaceOffset[face][1],
                                              clicked.z + kFaceOffset[face][2] };
        if (!inside_world(target.y)) {
            return;
        }
        if (state_at(target) != air_state_) {
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

    void Connection::refresh_held_item() {
        const std::int32_t hotbar_slot = 36 + carried_slot_;
        const auto it = creative_slots_.find(hotbar_slot);
        held_item_id_ = (it == creative_slots_.end()) ? 0 : it->second;
        LOG_DEBUG(Network, "held item is now {} (hotbar slot {})", held_item_id_, carried_slot_);
    }

} // namespace terila::network
