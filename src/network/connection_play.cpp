#include "network/connection.hpp"

#include "protocol/block.hpp"
#include "protocol/entity.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/play.hpp"

#include <cstring>

namespace terila::network {

    // F3+F4 asks the server for a gamemode; the answer is the game event that carries
    // the new mode plus fresh abilities, so creative flight and spectator flying match.
    void Connection::handle_change_gamemode(std::int32_t requested) {
        if (requested < 0 || requested > 3) {
            LOG_WARN(Network, "{} asked for invalid gamemode {}", username_, requested);
            return;
        }
        if (requested == gamemode_) {
            return;
        }

        gamemode_ = requested;
        LOG_INFO(Network, "{} switched to gamemode {}", username_, gamemode_);
        send_gamemode_state();
        if (hub_) {
            hub_->move(entity_id_,
                PlayerHub::Position{ player_x_, player_y_, player_z_, yaw_, pitch_ });
        }
    }

    void Connection::send_gamemode_state() {
        std::vector<std::uint8_t> out;

        // Entity event 28 raises this player's permission level on the client, which is
        // what makes the F3+F4 switcher usable outside singleplayer.
        if (terila::protocol::write_entity_event(out, entity_id_, 28)) {
            send(out);
        }
        out.clear();

        if (terila::protocol::write_game_event(out, 3, static_cast<float>(gamemode_))) {
            send(out);
        }
        out.clear();

        std::uint8_t flags = 0;
        if (gamemode_ == 1) {
            flags = 0x0D;   // invulnerable | may fly | instabuild
        }
        else if (gamemode_ == 3) {
            flags = 0x07;   // invulnerable | flying | may fly
        }
        if (terila::protocol::write_player_abilities(out, flags)) {
            send(out);
        }
    }

    void Connection::handle_play(std::int32_t id, const std::vector<std::uint8_t>& payload) {
        const auto& ids = terila::protocol::packet_ids();

        if (id == ids.play_keep_alive_serverbound) {
            auto value = terila::protocol::read_int64(payload.data(), payload.size());
            if (!value.has_value()) {
                LOG_WARN(Network, "invalid keep_alive reply ({} bytes)", payload.size());
                return;
            }
            if (*value != pending_keep_alive_id_) {
                LOG_DEBUG(Network, "keep_alive reply id={} does not match sent id={}",
                    *value, pending_keep_alive_id_);
            }
            else {
                keep_alive_answered_ = true;
                LOG_DEBUG(Network, "keep_alive reply: id={}", *value);
            }
            return;
        }
        if (id == ids.play_container_click) {
            handle_container_click(payload);
            return;
        }
        if (id == ids.play_container_close_serverbound) {
            const auto window = terila::protocol::read_varint(payload.data(), payload.size());
            if (window.has_value()) {
                handle_container_close(window->value);
            }
            return;
        }
        if (id == ids.play_change_game_mode) {
            const auto requested = terila::protocol::read_varint(payload.data(), payload.size());
            if (!requested.has_value()) {
                LOG_WARN(Network, "invalid change_game_mode ({} bytes)", payload.size());
                return;
            }
            handle_change_gamemode(requested->value);
            return;
        }
        if (id == ids.play_pick_item_from_block) {
            LOG_DEBUG(Network, "pick_block packet received ({} bytes)", payload.size());
            const auto pos = terila::protocol::unpack_block_pos(payload.data(), payload.size());
            handle_pick_block(pos);
            return;
        }
        if (id == ids.play_set_carried_item) {
            if (payload.size() < 2) return;
            carried_slot_ = static_cast<std::int32_t>((payload[0] << 8) | payload[1]);
            refresh_held_item();
            return;
        }
        if (id == ids.play_set_creative_mode_slot) {
            if (payload.size() < 2) return;
            const std::int32_t slot = static_cast<std::int32_t>((payload[0] << 8) | payload[1]);

            const auto count = terila::protocol::read_varint(payload.data() + 2, payload.size() - 2);
            if (!count.has_value()) return;
            const auto item = terila::protocol::read_varint(
                payload.data() + 2 + count->consumed,
                payload.size() - 2 - count->consumed);

            creative_slots_[slot] = item.has_value() ? item->value : 0;
            set_inventory_slot(slot, item.has_value() ? item->value : 0,
                item.has_value() ? count->value : 0);
            refresh_held_item();
            return;
        }
        if (id == ids.play_player_action) {
            const auto action = terila::protocol::read_varint(payload.data(), payload.size());
            if (!action.has_value() || payload.size() < action->consumed + 8) {
                LOG_WARN(Network, "invalid player_action ({} bytes)", payload.size());
                return;
            }

            const auto pos = terila::protocol::unpack_block_pos(
                payload.data() + action->consumed, payload.size() - action->consumed);

            // Face is one byte, then the sequence varint.
            const std::size_t sequence_offset = action->consumed + 8 + 1;
            const auto sequence = terila::protocol::read_varint(
                payload.data() + sequence_offset, payload.size() - sequence_offset);

            // 2 = finished destroying. A creative client only sends 0 (start destroy)
            // because it breaks the block instantly, so that counts as a break too —
            // otherwise the server keeps the block and re-sends it later.
            const bool finished = action->value == 2 || (action->value == 0 && instant_break());
            if (finished) {
                handle_block_break(pos, sequence.has_value() ? sequence->value : 0);
            }
            return;
        }
        if (id == ids.play_use_item_on) {
            const auto hand = terila::protocol::read_varint(payload.data(), payload.size());
            if (!hand.has_value() || payload.size() < hand->consumed + 8) {
                LOG_WARN(Network, "invalid use_item_on ({} bytes)", payload.size());
                return;
            }

            const auto pos = terila::protocol::unpack_block_pos(
                payload.data() + hand->consumed, payload.size() - hand->consumed);

            std::size_t offset = hand->consumed + 8;
            const auto face = terila::protocol::read_varint(
                payload.data() + offset, payload.size() - offset);
            if (!face.has_value()) return;

            // Cursor position (three floats) and two booleans precede the sequence.
            offset += face->consumed + 12 + 2;
            if (offset > payload.size()) return;
            const auto sequence = terila::protocol::read_varint(
                payload.data() + offset, payload.size() - offset);

            handle_block_place(pos, face->value,
                sequence.has_value() ? sequence->value : 0);
            return;
        }
        if (id == ids.play_attack) {
            const auto target = terila::protocol::read_varint(payload.data(), payload.size());
            if (!target.has_value()) {
                LOG_WARN(Network, "invalid attack packet ({} bytes)", payload.size());
                return;
            }

            const auto* mob = mobs_.find(target->value);
            if (mob == nullptr) {
                LOG_DEBUG(Network, "attack on unknown entity id={}", target->value);
                return;
            }

            const float away = terila::entity::yaw_between(
                player_x_, player_z_, mob->x, mob->z);
            mobs_.damage(target->value, kPlayerAttackDamage, away);
            return;
        }
        if (id == ids.play_move_player_pos || id == ids.play_move_player_pos_rot) {
            const auto x = terila::protocol::read_double(payload.data(), payload.size(), 0);
            const auto y = terila::protocol::read_double(payload.data(), payload.size(), 8);
            const auto z = terila::protocol::read_double(payload.data(), payload.size(), 16);
            if (x.has_value() && y.has_value() && z.has_value()) {
                player_x_ = *x;
                player_y_ = *y;
                player_z_ = *z;
                LOG_TRACE(Network, "player moved to ({:.2f}, {:.2f}, {:.2f})", *x, *y, *z);
                update_player_chunk(*x, *z);
                if (id == ids.play_move_player_pos_rot && payload.size() >= 32) {
                    const auto read_angle = [&payload](std::size_t offset) {
                        const std::uint32_t bits =
                            (static_cast<std::uint32_t>(payload[offset + 0]) << 24) |
                            (static_cast<std::uint32_t>(payload[offset + 1]) << 16) |
                            (static_cast<std::uint32_t>(payload[offset + 2]) << 8) |
                            static_cast<std::uint32_t>(payload[offset + 3]);
                        float value = 0.0f;
                        std::memcpy(&value, &bits, sizeof(value));
                        return value;
                    };
                    yaw_ = read_angle(24);
                    pitch_ = read_angle(28);
                }
                if (hub_) {
                    hub_->move(entity_id_,
                        PlayerHub::Position{ player_x_, player_y_, player_z_, yaw_, pitch_ });
                }
            }
            return;
        }
        LOG_DEBUG(Network, "unhandled play packet id=0x{:02X} ({} bytes)", id, payload.size());
    }

} // namespace terila::network
