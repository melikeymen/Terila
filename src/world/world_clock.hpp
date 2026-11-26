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

        // The day's sky light level as the game's timeline draws it: fifteen from just after
        // sunrise until dusk, four through the night, with a linear ramp on either side.
        // Level#getSkyDarken is fifteen minus this, and that number is what the natural
        // spawner subtracts from the stored sky light before comparing it to its limit.
        static constexpr float kDaySkyLight = 15.0f;
        static constexpr float kNightSkyLight = 4.0f;

        float sky_light_level() const noexcept {
            constexpr std::int64_t kDuskStart = 11867;
            constexpr std::int64_t kDuskEnd = 13670;
            constexpr std::int64_t kDawnStart = 22330;
            constexpr std::int64_t kDawnEnd = 24133;   // 133 of the next day
            std::int64_t t = time_of_day;
            if (t < kDawnEnd - kDayLength) {
                t += kDayLength;
            }
            const auto ramp = [](std::int64_t now, std::int64_t from, std::int64_t to) {
                const auto part = static_cast<float>(now - from) /
                    static_cast<float>(to - from);
                return kDaySkyLight + (kNightSkyLight - kDaySkyLight) * part;
            };
            if (t <= kDuskStart) return kDaySkyLight;
            if (t < kDuskEnd) return ramp(t, kDuskStart, kDuskEnd);
            if (t <= kDawnStart) return kNightSkyLight;
            return kDaySkyLight + (kNightSkyLight - kDaySkyLight) *
                (1.0f - static_cast<float>(t - kDawnStart) /
                    static_cast<float>(kDawnEnd - kDawnStart));
        }

        int sky_darken_level() const noexcept {
            return static_cast<int>(15.0f - sky_light_level());
        }

        // 0 at noon, 1 at midnight: the moon's share of the daylight, used for the threat
        // level a naturally spawned mob is created with.
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
