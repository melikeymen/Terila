#pragma once

#include "item/components.hpp"
#include "protocol/entity.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/player_info.hpp"
#include "world/dimension_kind.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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
            // Players only see each other inside one dimension.
            terila::world::DimensionKind dimension = terila::world::DimensionKind::Overworld;
        };

        // Handing an item over is not a packet: the hub only knows how to ask the
        // connection that owns the inventory, so a member registers a giver when it joins
        // and /give with a selector walks this list.
        using Giver = std::function<bool(std::int32_t item_id, std::int32_t count,
            const terila::item::ComponentsPtr& components)>;

        // A mob hit lands on a player, and only the connection that owns that player can
        // apply it: its health, its health packet and its death screen all live there.
        using Damager = std::function<bool(float amount, const std::string& cause)>;

        // /kill does not go through the damage rules; the owning connection applies the
        // death itself, which is what lets it reach a creative player too.
        using Killer = std::function<bool(const std::string& cause)>;

        // /spawnpoint names a point the owning connection keeps for its own respawn.
        using SpawnSetter = std::function<bool(double x, double y, double z, float yaw,
            float pitch)>;

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
            ids_dirty_ = true;

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

        // A command changes another player, and only the connection that owns that player
        // can apply the change: the same reason the giver and the damager exist.
        using Teleporter = std::function<bool(double x, double y, double z, float yaw,
            float pitch)>;
        // A move into another dimension, which only the owning connection can carry out.
        using Traveler = std::function<bool(terila::world::DimensionKind kind, double x,
            double y, double z, float yaw, float pitch)>;
        using Messenger = std::function<bool(std::string_view text)>;
        using ModeSetter = std::function<bool(int mode)>;

        void set_teleporter(std::int32_t entity_id, Teleporter teleporter)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end()) {
                it->second.teleport = std::move(teleporter);
            }
        }

        void set_traveler(std::int32_t entity_id, Traveler traveler)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end()) {
                it->second.travel = std::move(traveler);
            }
        }

        bool travel_to(std::int32_t entity_id, terila::world::DimensionKind kind, double x,
            double y, double z, float yaw, float pitch)
        {
            const auto it = players_.find(entity_id);
            if (it == players_.end() || !it->second.travel) {
                return false;
            }
            const bool moved = it->second.travel(kind, x, y, z, yaw, pitch);
            if (moved) {
                it->second.position.dimension = kind;
            }
            return moved;
        }

        void set_messenger(std::int32_t entity_id, Messenger messenger)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end()) {
                it->second.message = std::move(messenger);
            }
        }

        void set_mode_setter(std::int32_t entity_id, ModeSetter setter)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end()) {
                it->second.set_mode = std::move(setter);
            }
        }

        bool teleport_to(std::int32_t entity_id, double x, double y, double z, float yaw,
            float pitch)
        {
            const auto it = players_.find(entity_id);
            return it != players_.end() && it->second.teleport &&
                it->second.teleport(x, y, z, yaw, pitch);
        }

        bool message_to(std::int32_t entity_id, std::string_view text)
        {
            const auto it = players_.find(entity_id);
            return it != players_.end() && it->second.message && it->second.message(text);
        }

        bool set_gamemode_to(std::int32_t entity_id, int mode)
        {
            const auto it = players_.find(entity_id);
            if (it == players_.end() || !it->second.set_mode) {
                return false;
            }
            if (!it->second.set_mode(mode)) {
                return false;
            }
            it->second.gamemode = mode;
            return true;
        }

        // Replaces the giver of a member that is already in the world.
        void set_giver(std::int32_t entity_id, Giver giver)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end()) {
                it->second.give = std::move(giver);
            }
        }

        void set_damager(std::int32_t entity_id, Damager damager)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end()) {
                it->second.hurt = std::move(damager);
            }
        }

        void set_killer(std::int32_t entity_id, Killer killer)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end()) {
                it->second.slay = std::move(killer);
            }
        }

        void set_spawn_setter(std::int32_t entity_id, SpawnSetter setter)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end()) {
                it->second.set_spawn = std::move(setter);
            }
        }

        bool set_spawn_location(std::int32_t entity_id, double x, double y, double z,
            float yaw, float pitch)
        {
            const auto it = players_.find(entity_id);
            return it != players_.end() && it->second.set_spawn &&
                it->second.set_spawn(x, y, z, yaw, pitch);
        }

        // Applies an unconditional death. False when nobody with that id is online, or when
        // the connection behind it is already gone.
        bool kill(std::int32_t entity_id, const std::string& cause)
        {
            const auto it = players_.find(entity_id);
            return it != players_.end() && it->second.slay && it->second.slay(cause);
        }

        // Applies a mob hit. False when nobody with that id is online (or the player is
        // gone, e.g. disconnected between the tick and the delivery).
        bool damage_player(std::int32_t entity_id, float amount, const std::string& cause)
        {
            const auto it = players_.find(entity_id);
            return it != players_.end() && it->second.hurt && it->second.hurt(amount, cause);
        }

        // Every joined player receives the packet, e.g. the world clock once a second.
        void broadcast(const std::vector<std::uint8_t>& packet)
        {
            for (const auto id : member_list()) {
                send_to(id, packet);
            }
        }

        // /execute runs a command in another player's context, so a member also has to
        // expose the way to run one, for the same reason it exposes its giver.
        using Runner = std::function<void(const std::string&)>;

        void set_runner(std::int32_t entity_id, Runner runner)
        {
            const auto it = players_.find(entity_id);
            if (it != players_.end()) {
                it->second.run = std::move(runner);
            }
        }

        // Runs a command as that member. False when nobody with that id is online.
        bool run_as(std::int32_t entity_id, const std::string& command) const
        {
            const auto it = players_.find(entity_id);
            if (it == players_.end() || !it->second.run) {
                return false;
            }
            it->second.run(command);
            return true;
        }

        // Every player in the world, self included. A copy: running a command can end a
        // connection and rebuild the stored list.
        std::vector<std::int32_t> members() const { return member_list(); }

        // A player as a selector sees it: the id, the name and where distance= and sort=
        // are measured from.
        struct MemberInfo {
            std::int32_t entity = 0;
            std::string name;
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            float yaw = 0.0f;
            float pitch = 0.0f;
            int gamemode = 0;
            terila::world::DimensionKind dimension = terila::world::DimensionKind::Overworld;
        };

        std::vector<MemberInfo> member_info() const
        {
            std::vector<MemberInfo> out;
            out.reserve(players_.size());
            for (const auto& [id, member] : players_) {
                out.push_back(MemberInfo{ id, member.name, member.position.x,
                    member.position.y, member.position.z, member.position.yaw,
                    member.position.pitch, member.gamemode, member.position.dimension });
            }
            return out;
        }

        // A player named in a command, matched without regard to case. -1 when nobody is
        // called that.
        std::int32_t find_by_name(std::string_view name) const
        {
            if (name.empty()) {
                return -1;
            }
            const auto same = [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                    std::tolower(static_cast<unsigned char>(b));
            };
            for (const auto& [id, member] : players_) {
                if (name.size() == member.name.size() &&
                    std::equal(name.begin(), name.end(), member.name.begin(), same)) {
                    return id;
                }
            }
            return -1;
        }

        // Every player in the world, self included. Returns how many were served.
        //
        // Handing an item over can end a connection (a full send fails and the peer is
        // dropped), so the ids are collected first and the map is looked into again for
        // each of them; iterating the map itself would be walking freed nodes.
        std::size_t give_to_all(std::int32_t item_id, std::int32_t count,
            const terila::item::ComponentsPtr& components)
        {
            const auto ids = member_list();
            std::size_t delivered = 0;
            for (const auto id : ids) {
                if (give_to(id, item_id, count, components)) {
                    ++delivered;
                }
            }
            return delivered;
        }

        bool give_to(std::int32_t entity_id, std::int32_t item_id, std::int32_t count,
            const terila::item::ComponentsPtr& components)
        {
            const auto it = players_.find(entity_id);
            return it != players_.end() && it->second.give &&
                it->second.give(item_id, count, components);
        }

        // The closest player to a point, which is what @p means.
        std::int32_t nearest_to(double x, double z) const
        {
            std::int32_t best = -1;
            double best_distance = 0.0;
            for (const auto& [id, member] : players_) {
                const double dx = member.position.x - x;
                const double dz = member.position.z - z;
                const double distance = dx * dx + dz * dz;
                if (best < 0 || distance < best_distance) {
                    best = id;
                    best_distance = distance;
                }
            }
            return best;
        }

        // One player picked at random, which is what @r means. -1 when nobody is online.
        std::int32_t random_member() const
        {
            if (players_.empty()) {
                return -1;
            }

            const auto ids = member_list();
            return ids[random() % ids.size()];
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
            ids_dirty_ = true;

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

            // The list is walked in place: this runs for every movement packet, and the
            // copy of the member list cost an allocation each time. A send inside
            // sync_pair may drop a connection, which only marks the list dirty; the
            // vector itself is rebuilt lazily by the next call.
            for (const auto other_id : member_list()) {
                if (other_id == entity_id) {
                    continue;
                }
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
            Giver give;
            Runner run;
            Damager hurt;
            Killer slay;
            SpawnSetter set_spawn;
            Teleporter teleport;
            Traveler travel;
            Messenger message;
            ModeSetter set_mode;
        };

        // The member list changes only on join and leave, but it is needed on every move
        // packet; rebuilding it from the map each time allocated a vector per packet.
        const std::vector<std::int32_t>& member_list() const
        {
            if (ids_dirty_) {
                ids_.clear();
                ids_.reserve(players_.size());
                for (const auto& [id, member] : players_) {
                    ids_.push_back(id);
                }
                ids_dirty_ = false;
            }
            return ids_;
        }

        std::vector<std::int32_t> member_ids(std::int32_t except) const
        {
            std::vector<std::int32_t> others;
            others.reserve(players_.size());
            for (const auto id : member_list()) {
                if (id != except) {
                    others.push_back(id);
                }
            }
            return others;
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
            if (viewer.dimension != target.dimension) {
                return false;
            }
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

        // xorshift: /give @r decides who gets an item, and a rotation would hand it to the
        // same players in the same order every time.
        static std::uint64_t random()
        {
            static thread_local std::uint64_t state =
                static_cast<std::uint64_t>(std::chrono::steady_clock::now()
                    .time_since_epoch().count()) | 1u;
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }

        int view_distance_ = 10;
        std::int32_t player_type_id_ = 159;   // registry id of minecraft:player
        std::unordered_map<std::int32_t, Member> players_;
        mutable std::vector<std::int32_t> ids_;
        mutable bool ids_dirty_ = true;
        // viewer -> target -> last position this viewer was told about
        std::unordered_map<std::int32_t, std::unordered_map<std::int32_t, Position>> known_;
    };

}
