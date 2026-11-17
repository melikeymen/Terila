#pragma once

// The overworld day cycle: 24000 ticks, twenty of them per second, so one day lasts twenty
// minutes. Everything that asks "is it dark" reads this clock, and the natural spawner's
// monster rule hangs off it.

#include <cstdint>

namespace terila::world {

    struct WorldClock {
        static constexpr std::int64_t kTicksPerSecond = 20;
        static constexpr std::int64_t kDayLength = 24000;
        static constexpr std::int64_t kSunrise = 23000;
        static constexpr std::int64_t kNoon = 6000;
        static constexpr std::int64_t kSunset = 12000;
        static constexpr std::int64_t kMidnight = 18000;
        // Vanilla turns monsters on when the sky is dark enough, which starts a little after
        // sunset and ends a little before sunrise.
        static constexpr std::int64_t kNightStart = 13000;
        static constexpr std::int64_t kNightEnd = 23000;

        std::int64_t game_time = 0;     // total ticks the world has run
        std::int64_t time_of_day = 0;   // 0..23999, where 0 is sunrise

        void tick(std::int64_t ticks = 1) {
            game_time += ticks;
            set_time_of_day(time_of_day + ticks);
        }

        void set_time_of_day(std::int64_t ticks) {
            time_of_day = ((ticks % kDayLength) + kDayLength) % kDayLength;
        }

        void set_game_time(std::int64_t ticks) { game_time = ticks; }

        bool night() const noexcept {
            return time_of_day >= kNightStart && time_of_day < kNightEnd;
        }

        // 0 at noon, 1 at midnight: Level#getSkyDarken, used by the spawner to decide whether
        // a surface column counts as dark.
        float sky_darken() const noexcept {
            double darken = 1.0 -
                (std::cos(static_cast<double>(time_of_day) * 2.0 * 3.14159265358979323846 /
                    static_cast<double>(kDayLength)) + 1.0) * 0.5;
            darken = 1.0 - darken;          // 0 at midnight, 1 at noon
            darken = 0.2 + darken * 0.8;    // never fully bright
            return static_cast<float>(1.0 - darken);
        }
    };

} // namespace terila::world
