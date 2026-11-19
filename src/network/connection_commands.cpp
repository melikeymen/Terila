#include "network/connection.hpp"

#include "commands/dispatch.hpp"
#include "commands/text.hpp"
#include "protocol/chat.hpp"
#include "protocol/player_state.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace terila::network {

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

    // Every command reaches the game through this context: the selector module gets the
    // player list, /give the inventory, /time the clock and /execute the nesting runner.
    commands::Context Connection::command_context() {
        commands::Context context;
        context.self = commands::Target{ entity_id_, username_, player_x_, player_y_, player_z_ };

        context.players = [this]() -> std::vector<commands::Target> {
            std::vector<commands::Target> out;
            if (hub_ != nullptr) {
                for (const auto& member : hub_->member_info()) {
                    out.push_back(commands::Target{ member.entity, member.name, member.x,
                        member.y, member.z });
                }
            }
            if (out.empty()) {
                out.push_back(commands::Target{ entity_id_, username_, player_x_, player_y_,
                    player_z_ });
            }
            return out;
            };

        context.reply = [this](std::string_view text) { send_system_message(text); };

        context.give = [this](std::int32_t entity, std::int32_t item_id, std::int32_t count,
            const terila::item::ComponentsPtr& components) {
            if (entity == entity_id_ || hub_ == nullptr) {
                return give_item(item_id, count, components);
            }
            return hub_->give_to(entity, item_id, count, components);
            };

        context.run = [this](std::string_view line) { run_command(line); };

        context.run_as = [this](std::int32_t entity, std::string_view line) {
            if (entity == entity_id_) {
                run_command(line);
                return true;
            }
            return hub_ != nullptr && hub_->run_as(entity, std::string(line));
            };

        context.item_ids = &data_.item_ids;
        context.component_ids = &component_ids_;

        if (clock_ != nullptr) {
            time_api_.game_time = [this] { return clock_->game_time; };
            time_api_.time_of_day = [this] { return clock_->time_of_day; };
            time_api_.night = [this] { return clock_->night(); };
            time_api_.set = [this](std::int64_t ticks) { clock_->set_time_of_day(ticks); };
            time_api_.add = [this](std::int64_t ticks) { clock_->tick(ticks); };
            // Everyone's sky moves at once, so the new time is broadcast instead of waiting
            // for the next one-second sync.
            time_api_.sync = [this] {
                std::vector<std::uint8_t> out;
                if (terila::protocol::write_set_time(out, clock_->game_time,
                        clock_->time_of_day)) {
                    if (hub_ != nullptr) {
                        hub_->broadcast(out);
                    }
                    else {
                        send(out);
                    }
                }
                };
            context.time = &time_api_;
        }
        return context;
    }

    // A command that runs another command shares one guard, so
    // /execute as @s run execute as @s run ... cannot recurse until the stack is gone.
    void Connection::run_command(std::string_view command) {
        if (command_depth_ >= kMaxCommandDepth) {
            send_system_message("Too many nested commands");
            return;
        }
        ++command_depth_;
        handle_command(std::string(commands::trim(command)));
        --command_depth_;
    }

    void Connection::handle_command(const std::string& command) {
        const auto context = command_context();
        if (commands::dispatch(context, command)) {
            return;
        }
        send_system_message(
            "Commands: /give [@a|@s|@p|@r] <item>[<component>=<value>,...] [count], "
            "/execute [as <targets>] run <command>, /time set|add|query <value>");
    }

} // namespace terila::network
