#include "network/connection.hpp"

#include "commands/registry.hpp"
#include "entity/difficulty.hpp"
#include "entity/mob_data.hpp"
#include "protocol/chat.hpp"
#include "protocol/play.hpp"
#include "protocol/player_state.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace terila::network {

    // The game side of the command module. Every effect a command asks for lands here: the
    // player itself is served locally, anybody else through the hub, and the world through
    // the mob manager the connection already holds.
    class CommandBridge final : public terila::commands::GameApi {
    public:
        explicit CommandBridge(Connection& connection) : connection_(connection) {}

        std::vector<terila::commands::Target> players() const override {
            std::vector<terila::commands::Target> out;
            if (connection_.hub_) {
                for (const auto& member : connection_.hub_->member_info()) {
                    out.push_back(terila::commands::Target{ member.entity, member.name,
                        member.x, member.y, member.z, member.yaw, member.pitch });
                }
            }
            if (out.empty()) {
                out.push_back(self());
            }
            return out;
        }

        bool give(std::int32_t entity, std::int32_t item_id, std::int32_t count,
            const terila::item::ComponentsPtr& components) override {
            if (entity == connection_.entity_id_ || connection_.hub_ == nullptr) {
                return connection_.give_item(item_id, count, components);
            }
            return connection_.hub_->give_to(entity, item_id, count, components);
        }

        bool teleport(std::int32_t entity, double x, double y, double z, float yaw,
            float pitch) override {
            if (entity == connection_.entity_id_ || connection_.hub_ == nullptr) {
                return connection_.teleport_player(x, y, z, yaw, pitch);
            }
            return connection_.hub_->teleport_to(entity, x, y, z, yaw, pitch);
        }

        // `execute in <dimension> run tp` lands here. The local player is moved by this
        // connection; anybody else through the hub, which asks the connection that owns
        // him. False when the name is not a dimension this build has, or when the player
        // already stands there and only needs the plain teleport.
        bool teleport_in(std::int32_t entity, std::string_view dimension, double x, double y,
            double z, float yaw, float pitch) override {
            terila::world::DimensionKind kind{};
            if (!terila::world::worldgen::dimension_kind_of(dimension, kind)) {
                return false;
            }
            if (entity == connection_.entity_id_ || connection_.hub_ == nullptr) {
                return connection_.enter_dimension(kind, x, y, z, yaw, pitch);
            }
            return connection_.hub_->travel_to(entity, kind, x, y, z, yaw, pitch);
        }

        bool set_gamemode(std::int32_t entity, int mode) override {
            if (entity == connection_.entity_id_ || connection_.hub_ == nullptr) {
                return connection_.apply_gamemode(mode);
            }
            return connection_.hub_->set_gamemode_to(entity, mode);
        }

        int gamemode(std::int32_t entity) const override {
            if (entity == connection_.entity_id_ || connection_.hub_ == nullptr) {
                return connection_.gamemode_;
            }
            for (const auto& member : connection_.hub_->member_info()) {
                if (member.entity == entity) {
                    return member.gamemode;
                }
            }
            return -1;
        }

        bool summon(std::string_view type, double x, double y, double z) override {
            return connection_.summon_entity(type, x, y, z);
        }

        bool summonable(std::string_view type) const override {
            return terila::entity::data::find_mob_by_name(std::string(type)) != nullptr;
        }

        bool tell(std::int32_t entity, std::string_view text) override {
            if (entity == connection_.entity_id_ || connection_.hub_ == nullptr) {
                return connection_.tell_player(text);
            }
            return connection_.hub_->message_to(entity, text);
        }

        // The player list plus the mobs this connection already owns, so a selector written
        // as @e can name a zombie as readily as a player.
        std::vector<terila::commands::Target> entities() const override {
            std::vector<terila::commands::Target> out = players();
            for (const auto& mob : connection_.mobs_.mobs()) {
                if (mob.dead) {
                    continue;
                }
                out.push_back(terila::commands::Target{ mob.id, mob.type_name, mob.x, mob.y,
                    mob.z, mob.yaw, mob.pitch });
            }
            return out;
        }

        std::size_t broadcast(std::string_view text) override {
            std::size_t reached = 0;
            if (connection_.tell_player(text)) {
                ++reached;
            }
            if (connection_.hub_) {
                for (const auto id : connection_.hub_->members()) {
                    if (id == connection_.entity_id_) {
                        continue;
                    }
                    if (connection_.hub_->message_to(id, text)) {
                        ++reached;
                    }
                }
            }
            return reached;
        }

        bool kill(std::int32_t entity, std::string_view cause) override {
            if (connection_.mobs_.find(entity) != nullptr) {
                return connection_.mobs_.damage(entity, 100000, 0.0f);
            }
            const std::string reason(cause);
            if (entity == connection_.entity_id_ || connection_.hub_ == nullptr) {
                return connection_.kill_player(reason);
            }
            return connection_.hub_->kill(entity, reason);
        }

        std::int64_t seed() const override { return connection_.world_seed_; }
        int max_players() const override { return connection_.max_players_; }

        bool damage(std::int32_t entity, float amount, std::string_view cause) override {
            if (connection_.mobs_.find(entity) != nullptr) {
                return connection_.mobs_.damage(entity,
                    static_cast<std::int32_t>(std::max(1.0f, amount)), 0.0f);
            }
            const std::string reason(cause);
            if (entity == connection_.entity_id_ || connection_.hub_ == nullptr) {
                return connection_.damage_player(amount, reason);
            }
            return connection_.hub_->damage_player(entity, amount, reason);
        }

        int difficulty() const override {
            return static_cast<int>(*connection_.world_difficulty_);
        }

        bool set_difficulty(int level) override {
            if (level < 0 || level > 3 || connection_.world_difficulty_ == nullptr) {
                return false;
            }
            *connection_.world_difficulty_ = static_cast<terila::entity::Difficulty>(level);
            std::vector<std::uint8_t> out;
            if (!terila::protocol::write_change_difficulty(out,
                    static_cast<std::uint8_t>(level), false)) {
                return false;
            }
            if (connection_.hub_) {
                connection_.hub_->broadcast(out);
            }
            else {
                connection_.send(out);
            }
            return true;
        }

        int default_gamemode() const override {
            return connection_.default_gamemode_ ? *connection_.default_gamemode_ : 0;
        }

        bool set_default_gamemode(int mode) override {
            if (mode < 0 || mode > 3 || connection_.default_gamemode_ == nullptr) {
                return false;
            }
            *connection_.default_gamemode_ = mode;
            return true;
        }

        std::int32_t block_state_at(std::int32_t x, std::int32_t y,
            std::int32_t z) const override {
            const terila::world::BlockPos pos{ x, y, z };
            if (!connection_.inside_world(y)) {
                return -1;
            }
            return connection_.state_at(pos);
        }

        bool set_block_state(std::int32_t x, std::int32_t y, std::int32_t z,
            std::int32_t state) override {
            return connection_.set_block_state(terila::world::BlockPos{ x, y, z }, state);
        }

        void drop_block(std::int32_t x, std::int32_t y, std::int32_t z,
            std::int32_t state) override {
            connection_.spawn_block_drop(terila::world::BlockPos{ x, y, z }, state);
        }

        bool set_spawn_point(std::int32_t entity, double x, double y, double z, float yaw,
            float pitch) override {
            if (entity == connection_.entity_id_ || connection_.hub_ == nullptr) {
                return connection_.apply_spawn_point(x, y, z, yaw, pitch);
            }
            return connection_.hub_->set_spawn_location(entity, x, y, z, yaw, pitch);
        }

        bool set_world_spawn(double x, double y, double z, float yaw, float pitch) override {
            if (connection_.world_spawn_ == nullptr) {
                return false;
            }
            connection_.world_spawn_->x = x;
            connection_.world_spawn_->y = y;
            connection_.world_spawn_->z = z;
            connection_.world_spawn_->yaw = yaw;
            connection_.world_spawn_->pitch = pitch;
            connection_.world_spawn_->exact = true;
            return true;
        }

        std::int64_t game_time() const override {
            return connection_.clock_ ? connection_.clock_->game_time : 0;
        }

        std::int64_t time_of_day() const override {
            return connection_.clock_ ? connection_.clock_->time_of_day : 0;
        }

        bool night() const override {
            return connection_.clock_ != nullptr && connection_.clock_->night();
        }

        void set_time(std::int64_t ticks) override {
            if (connection_.clock_) {
                connection_.clock_->set_time_of_day(ticks);
            }
        }

        void add_time(std::int64_t ticks) override {
            if (connection_.clock_) {
                connection_.clock_->tick(ticks);
            }
        }

        void sync_time() override {
            connection_.send_time();
        }

    private:
        terila::commands::Target self() const {
            return terila::commands::Target{ connection_.entity_id_, connection_.username_,
                connection_.player_x_, connection_.player_y_, connection_.player_z_,
                connection_.yaw_, connection_.pitch_ };
        }

        Connection& connection_;
    };

    void Connection::send_system_message(std::string_view text) {
        std::vector<std::uint8_t> out;
        if (terila::protocol::write_system_chat(out, text)) {
            send(out);
        }
    }

    bool Connection::give_item(std::int32_t item_id, std::int32_t count,
        const terila::item::ComponentsPtr& components) {
        return add_to_inventory(item_id, count, components);
    }

    bool Connection::teleport_player(double x, double y, double z, float yaw, float pitch) {
        if (state_ != State::Play || closed_) {
            return false;
        }

        player_x_ = x;
        player_y_ = y;
        player_z_ = z;
        yaw_ = yaw;
        pitch_ = pitch;

        std::vector<std::uint8_t> out;
        if (!terila::protocol::write_player_position(out, next_teleport_id_++, x, y, z, 0.0,
                0.0, 0.0, yaw, pitch)) {
            return false;
        }
        send(out);

        update_player_chunk(x, z);
        if (hub_) {
            hub_->move(entity_id_,
                PlayerHub::Position{ x, y, z, yaw, pitch, dimension_ });
        }
        return true;
    }

    bool Connection::apply_gamemode(int mode) {
        if (mode < 0 || mode > 3 || mode == gamemode_) {
            return false;
        }

        gamemode_ = mode;
        // A dig in progress belongs to the mode it started in.
        abort_breaking();
        LOG_INFO(Network, "{} switched to gamemode {}", username_, gamemode_);
        send_gamemode_state();
        if (hub_) {
            hub_->move(entity_id_,
                PlayerHub::Position{ player_x_, player_y_, player_z_, yaw_, pitch_, dimension_ });
        }
        return true;
    }

    bool Connection::apply_spawn_point(double x, double y, double z, float yaw, float pitch) {
        spawn_x_ = static_cast<std::int32_t>(std::floor(x));
        spawn_z_ = static_cast<std::int32_t>(std::floor(z));
        spawn_y_ = y;
        spawn_exact_ = true;
        yaw_ = yaw;
        pitch_ = pitch;
        return true;
    }

    bool Connection::tell_player(std::string_view text) {
        if (state_ != State::Play || closed_) {
            return false;
        }
        send_system_message(text);
        return true;
    }

    bool Connection::summon_entity(std::string_view type, double x, double y, double z) {
        return mobs_.spawn_at(std::string(type), x, y, z);
    }

    void Connection::handle_command(const std::string& command) {
        if (!registry_) {
            send_system_message("Commands are not available");
            return;
        }

        CommandBridge bridge(*this);
        terila::commands::Origin origin;
        origin.entity = entity_id_;
        origin.name = username_;
        origin.x = player_x_;
        origin.y = player_y_;
        origin.z = player_z_;
        origin.yaw = yaw_;
        origin.pitch = pitch_;
        // There is no permission store yet, so every joined player may run what the grammar
        // carries; the capability is checked at parse and at run time already.
        origin.capability = terila::commands::Capability::Owner;
        origin.reply = [this](std::string_view text) { send_system_message(text); };
        origin.game = &bridge;

        terila::commands::RunScope scope;
        registry_->run(origin, command, scope);
    }

} // namespace terila::network
