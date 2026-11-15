#pragma once

#include "protocol/entity.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/player_info.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terila::network {

    // Registry of the players that are currently in the world.
    //
    // A connection registers itself when it joins and unregisters when it closes,
    // so every client learns about the others (player list entry plus a player
    // entity) and receives their movement. The hub owns the per-viewer
    // bookkeeping, because it is the only place that knows which player entity a
    // given viewer already has; relative-move packets depend on that, and so does
    // not respawning a player every tick.
    class PlayerHub {
    public:
        using Sender = std::function<void(const std::vector<std::uint8_t>&)>;

        struct Position {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            float yaw = 0.0f;
            float pitch = 0.0f;
        };

        void set_view_distance(int chunks) { view_distance_ = chunks > 0 ? chunks : 10; }

        // minecraft:player is a registry entry too, and add_entity takes its id.
        void set_player_type_id(std::int32_t id) { player_type_id_ = id; }

        std::size_t count() const { return players_.size(); }

        void join(std::int32_t entity_id, const std::array<std::uint8_t, 16>& uuid,
            std::string name, int gamemode,
            const std::vector<protocol::ProfileProperty>& properties,
            const Position& position, Sender sender)
        {
            if (players_.find(entity_id) != players_.end()) {
                return;
            }

            Member member;
            member.uuid = uuid;
            member.name = name;
            member.gamemode = gamemode;
            member.properties = properties;
            member.position = position;
            member.send = std::move(sender);
            players_.emplace(entity_id, std::move(member));

            // The player list is sent once, for everyone, regardless of distance:
            // the tab list is not view dependent.
            std::vector<protocol::PlayerListEntry> for_newcomer;
            for_newcomer.reserve(players_.size());
            for (const auto& [id, other] : players_) {
                for_newcomer.push_back(protocol::PlayerListEntry{
                    other.uuid, other.name, other.gamemode, 0, true, other.properties });
            }

            std::vector<std::uint8_t> packet;
            if (protocol::write_player_info_add(packet, for_newcomer)) {
                send_to(entity_id, packet);
            }

            // Only the entities are range dependent. Ids are collected first so a
            // send that closes a connection cannot invalidate this iteration.
            const auto others = member_ids(entity_id);

            for (const auto other_id : others) {
                std::vector<std::uint8_t> info;
                const std::vector<protocol::PlayerListEntry> entry{
                    protocol::PlayerListEntry{ uuid, name, gamemode, 0, true, properties } };
                if (protocol::write_player_info_add(info, entry)) {
                    send_to(other_id, info);
                }

                sync_pair(entity_id, other_id);
                sync_pair(other_id, entity_id);
            }
        }

        void leave(std::int32_t entity_id)
        {
            const auto it = players_.find(entity_id);
            if (it == players_.end()) {
                return;
            }

            const auto uuid = it->second.uuid;
            players_.erase(it);
            known_.erase(entity_id);

            const auto others = member_ids(entity_id);
            for (const auto other_id : others) {
                auto viewer = known_.find(other_id);
                if (viewer != known_.end() && viewer->second.erase(entity_id) > 0) {
                    std::vector<std::uint8_t> remove;
                    if (protocol::write_remove_entity(remove, entity_id)) {
                        send_to(other_id, remove);
                    }
                }

                std::vector<std::uint8_t> info;
                if (protocol::write_player_info_remove(info, { uuid })) {
                    send_to(other_id, info);
                }
            }
        }

        void move(std::int32_t entity_id, const Position& position)
        {
            const auto it = players_.find(entity_id);
            if (it == players_.end()) {
                return;
            }
            it->second.position = position;

            const auto others = member_ids(entity_id);
            for (const auto other_id : others) {
                sync_pair(other_id, entity_id);   // the others see the mover
                sync_pair(entity_id, other_id);   // the mover sees them from here
            }
        }

    private:
        struct Member {
            std::array<std::uint8_t, 16> uuid{};
            std::string name;
            int gamemode = 0;
            std::vector<protocol::ProfileProperty> properties;
            Position position;
            Sender send;
        };

        std::vector<std::int32_t> member_ids(std::int32_t except) const
        {
            std::vector<std::int32_t> ids;
            ids.reserve(players_.size());
            for (const auto& [id, member] : players_) {
                if (id != except) {
                    ids.push_back(id);
                }
            }
            return ids;
        }

        void send_to(std::int32_t entity_id, const std::vector<std::uint8_t>& packet)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end() && it->second.send) {
                it->second.send(packet);
            }
        }

        bool in_range(const Position& viewer, const Position& target) const
        {
            const double range = static_cast<double>(view_distance_) * 16.0;
            const double dx = target.x - viewer.x;
            const double dz = target.z - viewer.z;
            return (dx * dx + dz * dz) <= range * range;
        }

        static std::int16_t to_short(double delta)
        {
            const double scaled = delta * 4096.0;
            if (scaled > 32767.0) return static_cast<std::int16_t>(32767);
            if (scaled < -32768.0) return static_cast<std::int16_t>(-32768);
            return static_cast<std::int16_t>(std::lround(scaled));
        }

        // Brings one viewer's view of one target up to date: spawn, despawn, or a
        // relative move. Deltas outside the 1/4096 short range would be lossy, so a
        // large jump despawns and immediately respawns at the new position.
        void sync_pair(std::int32_t viewer_id, std::int32_t target_id)
        {
            if (viewer_id == target_id) {
                return;
            }

            const auto viewer = players_.find(viewer_id);
            const auto target = players_.find(target_id);
            if (viewer == players_.end() || target == players_.end()) {
                return;
            }

            auto& seen = known_[viewer_id];
            const auto seen_it = seen.find(target_id);
            const bool visible = in_range(viewer->second.position, target->second.position);

            if (!visible) {
                if (seen_it == seen.end()) {
                    return;
                }
                seen.erase(seen_it);
                std::vector<std::uint8_t> remove;
                if (protocol::write_remove_entity(remove, target_id)) {
                    viewer->second.send(remove);
                }
                return;
            }

            const auto& pos = target->second.position;

            if (seen_it == seen.end()) {
                std::vector<std::uint8_t> spawn;
                if (protocol::write_add_entity(spawn, target_id, target->second.uuid, player_type_id_,
                        pos.x, pos.y, pos.z, pos.yaw, pos.pitch, pos.yaw)) {
                    viewer->second.send(spawn);
                }
                seen.emplace(target_id, pos);
                return;
            }

            const double dx = pos.x - seen_it->second.x;
            const double dy = pos.y - seen_it->second.y;
            const double dz = pos.z - seen_it->second.z;
            const bool turned = pos.yaw != seen_it->second.yaw || pos.pitch != seen_it->second.pitch;

            if (!turned && dx == 0.0 && dy == 0.0 && dz == 0.0) {
                return;
            }

            if (std::abs(dx) > 7.5 || std::abs(dy) > 7.5 || std::abs(dz) > 7.5) {
                seen.erase(seen_it);
                std::vector<std::uint8_t> remove;
                if (protocol::write_remove_entity(remove, target_id)) {
                    viewer->second.send(remove);
                }
                sync_pair(viewer_id, target_id);   // re-adds it at the new position
                return;
            }

            std::vector<std::uint8_t> move;
            if (protocol::write_move_entity_pos_rot(move, target_id,
                    to_short(dx), to_short(dy), to_short(dz), pos.yaw, pos.pitch, true)) {
                viewer->second.send(move);
            }
            seen_it->second = pos;
        }

        int view_distance_ = 10;
        std::int32_t player_type_id_ = 159;   // registry id of minecraft:player
        std::unordered_map<std::int32_t, Member> players_;
        // viewer -> target -> last position this viewer was told about
        std::unordered_map<std::int32_t, std::unordered_map<std::int32_t, Position>> known_;
    };

}
