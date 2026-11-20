#include "commands/registry.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>

namespace terila::commands {

    namespace {

        void set_and_report(Exec& exec, std::int64_t ticks, bool add) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return;
            }
            if (add) {
                origin.game->add_time(ticks);
            }
            else {
                origin.game->set_time(ticks);
            }
            origin.game->sync_time();
            origin.say(std::format("Set the time to {} ({})", origin.game->time_of_day(),
                origin.game->night() ? "night" : "day"));
        }

        Result run_ticks(Exec& exec) {
            const auto* ticks = exec.args.get<std::int64_t>("ticks");
            if (ticks == nullptr) {
                return Result{ 0, false };
            }
            set_and_report(exec, *ticks, false);
            return Result{ 1, true };
        }

        Result run_add(Exec& exec) {
            const auto* ticks = exec.args.get<std::int64_t>("ticks");
            if (ticks == nullptr) {
                return Result{ 0, false };
            }
            set_and_report(exec, *ticks, true);
            return Result{ 1, true };
        }

        Result run_query_daytime(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }
            const auto ticks = origin.game->time_of_day();
            origin.say(std::format("The time is {} ({})", ticks,
                origin.game->night() ? "night" : "day"));
            return Result{ static_cast<std::int32_t>(std::min<std::int64_t>(ticks, 2147483647)),
                true };
        }

        Result run_query_gametime(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }
            const auto ticks = origin.game->game_time();
            origin.say(std::format("The world has run {} ticks", ticks));
            return Result{ static_cast<std::int32_t>(std::min<std::int64_t>(ticks, 2147483647)),
                true };
        }

    } // namespace

    void install_time(Registry& registry) {
        Node& time = registry.root().literal("time");

        Node& set = time.literal("set");
        const std::pair<const char*, std::int64_t> markers[] = {
            { "day", 1000 }, { "noon", 6000 }, { "sunset", 12000 },
            { "night", 13000 }, { "midnight", 18000 }, { "sunrise", 23000 },
        };
        for (const auto& [name, ticks] : markers) {
            set.literal(name).executes([ticks](Exec& exec) {
                set_and_report(exec, ticks, false);
                return Result{ 1, true };
            });
        }
        set.argument("ticks", Parser::integer(0)).executes(run_ticks);

        time.literal("add").argument("ticks", Parser::integer(0)).executes(run_add);

        Node& query = time.literal("query");
        query.literal("daytime").executes(run_query_daytime);
        query.literal("gametime").executes(run_query_gametime);
    }

} // namespace terila::commands
