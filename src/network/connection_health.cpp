#include "network/connection.hpp"

#include "protocol/chat.hpp"
#include "protocol/play.hpp"
#include "protocol/player_state.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace terila::network {

    namespace {

        // Vanilla's ServerPlayer.doTick: health is clamped to [0, max] before it is sent,
        // because a client given 20.0001 draws a full heart row at 10.5.
        constexpr float kMaxHealth = 20.0f;
        constexpr float kMaxSaturation = 5.0f;
        constexpr std::int32_t kMaxFood = 20;

    } // namespace

    void Connection::send_health() {
        health_ = std::clamp(health_, 0.0f, kMaxHealth);
        food_ = std::clamp(food_, static_cast<std::int32_t>(0), kMaxFood);
        saturation_ = std::clamp(saturation_, 0.0f, kMaxSaturation);

        std::vector<std::uint8_t> out;
        if (terila::protocol::write_set_health(out, health_, food_, saturation_)) {
            send(out);
        }
    }

    // Damage types are not modelled yet, so the cause is only used for the death screen; the
    // armour calculation lives with the mobs, which own the attack that produced the number.
    bool Connection::damage_player(float amount, const std::string& cause) {
        if (!joined_ || dead_ || gamemode_ == 1 || gamemode_ == 3) {
            // Creative and spectator players are invulnerable, exactly like vanilla.
            return false;
        }
        if (amount <= 0.0f) {
            return false;
        }

        health_ -= amount;
        LOG_DEBUG(Network, "{} took {:.1f} damage from {} (hp {:.1f})", username_, amount,
            cause.empty() ? "a mob" : cause, std::max(health_, 0.0f));
        send_health();

        if (health_ > 0.0f) {
            return true;
        }

        // Dead: the client shows the death screen until it asks to respawn.
        dead_ = true;
        health_ = 0.0f;
        const std::string message = std::format("{} was slain by {}",
            username_.empty() ? "Player" : username_,
            cause.empty() ? "a mob" : cause);
        std::vector<std::uint8_t> out;
        if (terila::protocol::write_player_combat_kill(out, entity_id_, message)) {
            send(out);
        }
        LOG_INFO(Network, "{} died to {}", username_, cause.empty() ? "a mob" : cause);
        return true;
    }

    // The respawn button sends client_command with action 0. The client throws its world away
    // when it sees the respawn packet, so the chunks have to be streamed again from scratch.
    void Connection::respawn_player() {
        if (!dead_) {
            return;
        }

        health_ = kMaxHealth;
        food_ = kMaxFood;
        saturation_ = kMaxSaturation;
        dead_ = false;

        std::vector<std::uint8_t> out;
        if (terila::protocol::write_respawn(out, "minecraft:overworld",
                static_cast<std::int64_t>(world_seed_), static_cast<std::uint8_t>(gamemode_),
                sea_level_)) {
            send(out);
        }
        send_health();

        player_x_ = spawn_x_ + 0.5;
        player_z_ = spawn_z_ + 0.5;
        player_y_ = static_cast<double>(terrain_.surface_height(spawn_x_, spawn_z_) + 1);

        out.clear();
        if (terila::protocol::write_player_position(out, next_teleport_id_++, player_x_, player_y_,
                player_z_, 0.0, 0.0, 0.0, yaw_, pitch_)) {
            send(out);
        }

        if (hub_) {
            hub_->move(entity_id_,
                PlayerHub::Position{ player_x_, player_y_, player_z_, yaw_, pitch_ });
        }

        // The client dropped the whole world with the respawn packet, so every chunk has to
        // be streamed again; update_player_chunk would stay quiet because the column it is
        // asked about is the one it already believes it is centred on.
        center_cx_ = chunk_coordinate(spawn_x_);
        center_cz_ = chunk_coordinate(spawn_z_);
        sent_chunks_.clear();
        out.clear();
        if (terila::protocol::write_set_center_chunk(out, center_cx_, center_cz_)) {
            send(out);
        }
        queue_chunk_area(center_cx_, center_cz_);

        LOG_INFO(Network, "{} respawned at ({:.1f}, {:.1f}, {:.1f})", username_, player_x_,
            player_y_, player_z_);
    }

    void Connection::send_time() {
        if (clock_ == nullptr) {
            return;
        }
        std::vector<std::uint8_t> out;
        if (terila::protocol::write_set_time(out, clock_->game_time, clock_->time_of_day)) {
            send(out);
        }
    }

    // Twenty ticks between packets keeps the client's clock within a second of the server's;
    // a packet per tick would be twenty times the traffic for the same sky.
    void Connection::tick_time() {
        if (!joined_) {
            return;
        }

        // Vanilla's peaceful regeneration: a full food bar heals a heart every four seconds.
        if (!dead_ && health_ < kMaxHealth && food_ >= 18) {
            if (++regen_ticks_ >= 80) {
                regen_ticks_ = 0;
                health_ = std::min(kMaxHealth, health_ + 1.0f);
                send_health();
            }
        }
        else {
            regen_ticks_ = 0;
        }
        if (--time_sync_ticks_ > 0) {
            return;
        }
        time_sync_ticks_ = static_cast<int>(terila::world::WorldClock::kTicksPerSecond);
        send_time();
    }

    void Connection::handle_client_command(std::int32_t action) {
        // 0 is "perform respawn", 1 is "request stats", which this server has no answer for.
        if (action == 0) {
            respawn_player();
        }
    }

    // Right-clicking a mob feeds it when the held item is one of its foods.
    void Connection::handle_interact(const std::vector<std::uint8_t>& payload) {
        const auto interact = terila::protocol::read_interact(payload);
        if (!interact) {
            LOG_WARN(Network, "invalid interact packet ({} bytes)", payload.size());
            return;
        }

        if (mobs_.feed(interact->entity_id, held_item_name_)) {
            LOG_DEBUG(Network, "{} fed entity {} with {}", username_, interact->entity_id,
                held_item_name_);
        }
    }

} // namespace terila::network
