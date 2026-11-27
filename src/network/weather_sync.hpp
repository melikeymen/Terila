#pragma once

#include "protocol/play.hpp"
#include "world/weather.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace terila::network {

    // The game events that put a client's sky into the current weather: the start/stop
    // event drives the transition, and the two level events pin the exact amounts.
    inline std::vector<std::vector<std::uint8_t>> weather_packets(
        const terila::world::Weather& weather) {
        std::vector<std::vector<std::uint8_t>> packets;
        const auto emit = [&packets](std::uint8_t event, float value) {
            std::vector<std::uint8_t> out;
            if (terila::protocol::write_game_event(out, event, value)) {
                packets.push_back(std::move(out));
            }
        };

        if (weather.raining()) {
            emit(1, 0.0f);
            emit(7, weather.rain_level());
        }
        else {
            emit(2, 0.0f);
            emit(7, 0.0f);
        }
        emit(8, weather.thunder_level());
        return packets;
    }

} // namespace terila::network
