#include "network/connection.hpp"

#include "protocol/entity.hpp"
#include "protocol/item.hpp"
#include "protocol/play.hpp"

#include <cmath>
#include <cstdio>

namespace terila::network {

    namespace {

        std::int16_t delta_to_short(double delta) {
            const double scaled = delta * 4096.0;
            if (scaled > 32767.0) return static_cast<std::int16_t>(32767);
            if (scaled < -32768.0) return static_cast<std::int16_t>(-32768);
            return static_cast<std::int16_t>(std::lround(scaled));
        }

    }

    void Connection::send_entity_updates(const std::vector<terila::entity::Mob>& mobs) {
        if (state_ != State::Play || closed_) {
            return;
        }

        const double range = static_cast<double>(view_distance_) * 16.0;
        const double range_sq = range * range;

        auto& alive = alive_scratch_;
        alive.clear();
        alive.reserve(mobs.size());

        std::vector<std::uint8_t> out;

        for (const auto& mob : mobs) {
            alive.insert(mob.id);

            const double dx = mob.x - player_x_;
            const double dz = mob.z - player_z_;
            const bool in_range = (dx * dx + dz * dz) <= range_sq;

            const auto known = known_entities_.find(mob.id);

            if (!in_range) {
                if (known != known_entities_.end()) {
                    std::vector<std::uint8_t> remove;
                    if (terila::protocol::write_remove_entity(remove, mob.id)) {
                        send(remove);
                    }
                    known_entities_.erase(known);
                }
                continue;
            }

            if (known == known_entities_.end()) {
                if (terila::protocol::write_add_entity(out, mob.id, mob.uuid, mob.type_id,
                    mob.x, mob.y, mob.z, mob.yaw, mob.pitch, mob.head_yaw)) {
                    if (!logged_first_entity_) {
                        logged_first_entity_ = true;
                        std::string hex;
                        for (const auto byte : out) {
                            char buf[4];
                            std::snprintf(buf, sizeof(buf), "%02X ", byte);
                            hex += buf;
                        }
                        LOG_DEBUG(Entity, "add_entity type={} bytes={} [{}]",
                            mob.type_id, out.size(), hex);
                    }
                    send(out);
                }
                out.clear();
                known_entities_[mob.id] = KnownEntity{ mob.x, mob.y, mob.z, mob.yaw, mob.head_yaw };
                continue;
            }

            // Hurt feedback is per viewer: one animation per hit serial.
            if (mob.hurt_serial != known->second.hurt_serial) {
                known->second.hurt_serial = mob.hurt_serial;
                const float yaw = terila::entity::yaw_between(
                    player_x_, player_z_, mob.x, mob.z);
                if (terila::protocol::write_hurt_animation(out, mob.id, yaw)) {
                    send(out);
                }
                out.clear();
                if (terila::protocol::write_entity_event(out, mob.id, 2)) {
                    send(out);
                }
                out.clear();
            }

            if (mob.dead) {
                if (!known->second.death_sent) {
                    known->second.death_sent = true;
                    if (terila::protocol::write_entity_event(out, mob.id, 3)) {
                        send(out);
                    }
                    out.clear();
                }
                continue;
            }

            const auto to_short = [](double delta) {
                const double scaled = delta * 4096.0;
                if (scaled > 32767.0) return static_cast<std::int16_t>(32767);
                if (scaled < -32768.0) return static_cast<std::int16_t>(-32768);
                return static_cast<std::int16_t>(std::lround(scaled));
                };

            const auto dxs = to_short(mob.x - known->second.x);
            const auto dys = to_short(mob.y - known->second.y);
            const auto dzs = to_short(mob.z - known->second.z);

            const bool moved = dxs != 0 || dys != 0 || dzs != 0;
            const bool turned = mob.yaw != known->second.yaw;
            const bool head_turned = mob.head_yaw != known->second.head_yaw;

            if (!moved && !turned && !head_turned) {
                continue;
            }

            if (moved) {
                if (terila::protocol::write_move_entity_pos_rot(out, mob.id, dxs, dys, dzs,
                    mob.yaw, mob.pitch, mob.on_ground)) {
                    send(out);
                }
                out.clear();
            }
            else if (turned) {
                // The body has to turn, not only the head: a mob that turns in place and only
                // sends its head yaw keeps walking with its old body direction on the client.
                if (terila::protocol::write_move_entity_rot(out, mob.id, mob.yaw, mob.pitch,
                    mob.on_ground)) {
                    send(out);
                }
                out.clear();
            }
            if (head_turned) {
                if (terila::protocol::write_rotate_head(out, mob.id, mob.head_yaw)) {
                    send(out);
                }
                out.clear();
            }

            known->second.x = mob.x;
            known->second.y = mob.y;
            known->second.z = mob.z;
            known->second.yaw = mob.yaw;
            known->second.head_yaw = mob.head_yaw;
        }

        for (auto it = known_entities_.begin(); it != known_entities_.end();) {
            if (alive.find(it->first) != alive.end()) {
                ++it;
                continue;
            }

            std::vector<std::uint8_t> remove;
            if (terila::protocol::write_remove_entity(remove, it->first)) {
                send(remove);
            }
            it = known_entities_.erase(it);
        }
    }

    // Drops are streamed like mobs: spawn with the stack metadata, then relative moves
    // until the drop is collected, despawns or leaves the view distance.
    void Connection::send_drop_updates() {
        if (drops_ == nullptr || state_ != State::Play || closed_) {
            return;
        }

        const double range = static_cast<double>(view_distance_) * 16.0;
        const double range_sq = range * range;

        auto& alive = alive_scratch_;
        alive.clear();
        std::vector<std::uint8_t> out;

        for (const auto& drop : drops_->drops()) {
            alive.insert(drop.id);

            const double dx = drop.x - player_x_;
            const double dz = drop.z - player_z_;
            if (dx * dx + dz * dz > range_sq) {
                if (const auto known = known_drops_.find(drop.id); known != known_drops_.end()) {
                    std::vector<std::uint8_t> remove;
                    if (terila::protocol::write_remove_entity(remove, drop.id)) {
                        send(remove);
                    }
                    known_drops_.erase(known);
                }
                continue;
            }

            const auto known = known_drops_.find(drop.id);
            if (known == known_drops_.end()) {
                if (terila::protocol::write_add_entity(out, drop.id, drop.uuid, item_entity_type_,
                        drop.x, drop.y, drop.z, 0.0f, 0.0f, 0.0f)) {
                    send(out);
                }
                out.clear();
                if (terila::protocol::write_item_entity_data(out, drop.id, drop.item_id,
                        drop.count)) {
                    send(out);
                }
                out.clear();
                known_drops_[drop.id] = KnownDrop{ drop.x, drop.y, drop.z };
            }
            else {
                const auto dxs = delta_to_short(drop.x - known->second.x);
                const auto dys = delta_to_short(drop.y - known->second.y);
                const auto dzs = delta_to_short(drop.z - known->second.z);
                if (dxs != 0 || dys != 0 || dzs != 0) {
                    if (terila::protocol::write_move_entity_pos(out, drop.id, dxs, dys, dzs, false)) {
                        send(out);
                    }
                    out.clear();
                    known->second.x = drop.x;
                    known->second.y = drop.y;
                    known->second.z = drop.z;
                }
            }

            if (drop.collected_by != 0) {
                if (drop.collected_by == entity_id_) {
                    add_to_inventory(drop.item_id, drop.count);
                }
                if (drop.collected_by > 0 &&
                    terila::protocol::write_take_item_entity(out, drop.id, drop.collected_by,
                        drop.count)) {
                    send(std::move(out));
                    out.clear();
                }
                std::vector<std::uint8_t> remove;
                if (terila::protocol::write_remove_entity(remove, drop.id)) {
                    send(remove);
                }
                known_drops_.erase(drop.id);
            }
        }

        for (auto it = known_drops_.begin(); it != known_drops_.end();) {
            if (alive.find(it->first) != alive.end()) {
                ++it;
                continue;
            }

            std::vector<std::uint8_t> remove;
            if (terila::protocol::write_remove_entity(remove, it->first)) {
                send(remove);
            }
            it = known_drops_.erase(it);
        }
    }

} // namespace terila::network
