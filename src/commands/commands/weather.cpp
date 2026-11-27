#include "commands/registry.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <random>

namespace terila::commands {

    namespace {

        constexpr int kClear = 0;
        constexpr int kRain = 1;
        constexpr int kThunder = 2;

        std::int64_t random_duration() {
            // The game picks a duration between five and fifteen minutes when the command
            // names none.
            thread_local std::mt19937 rng(static_cast<std::uint32_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
            std::uniform_int_distribution<int> seconds(300, 900);
            return static_cast<std::int64_t>(seconds(rng)) * 20;
        }

        Result run_weather(Exec& exec, int kind) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            // Clear weather has nothing to count down, so a duration only matters for the
            // two wet states.
            std::int64_t ticks = 0;
            if (kind != kClear) {
                if (const auto* seconds = exec.args.get<std::int64_t>("duration")) {
                    ticks = *seconds * 20;
                }
                if (ticks <= 0) {
                    ticks = random_duration();
                }
            }

            if (!origin.game->set_weather(kind, ticks)) {
                origin.say("The weather could not be changed");
                return Result{ 0, false };
            }

            switch (kind) {
            case kRain:
                origin.say("Changing to rainy weather");
                break;
            case kThunder:
                origin.say("Changing to rain and thunder");
                break;
            default:
                origin.say("Changing to clear weather");
                break;
            }
            return Result{ 1, true };
        }

        void add_duration(Node& node, int kind) {
            node.executes([kind](Exec& exec) { return run_weather(exec, kind); });
            node.argument("duration", Parser::integer(0, true, 1000000))
                .executes([kind](Exec& exec) { return run_weather(exec, kind); });
        }

    } // namespace

    void install_weather(Registry& registry) {
        Node& weather = registry.root().literal("weather", Capability::Operator);
        add_duration(weather.literal("clear"), kClear);
        add_duration(weather.literal("rain"), kRain);
        add_duration(weather.literal("thunder"), kThunder);
    }

} // namespace terila::commands
