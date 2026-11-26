#pragma once

// The lighting and collision answers of every block state, read from the game's own block
// data. It lives apart from the rest of the registry loader because it needs nothing but the
// JSON reader, so the light engine's tests can check it without the whole data layer.

#include "data/input_store.hpp"
#include "data/registry_data.hpp"
#include "log/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace terila::data {

    inline void load_block_light(RegistryData& out, const std::string& path) {
        std::string text;
        if (!input::read_text(path, text)) {
            LOG_WARN(Data, "cannot open {}", path);
            return;
        }

        try {
            auto j = nlohmann::json::parse(text);
            if (!j.contains("data") || !j["data"].is_array()) {
                LOG_WARN(Data, "unexpected format in {} (no 'data' array)", path);
                return;
            }

            for (const auto& entry : j["data"]) {
                if (!entry.contains("id") || !entry["id"].is_string()) continue;

                BlockLightInfo info;
                info.emission = static_cast<std::uint8_t>(std::clamp(
                    entry.value("light_emission", 0), 0, 15));
                const bool solid_render = entry.value("is_solid_render", false);
                const bool permeable = entry.value("is_light_permeable", false);
                // A full render shape takes away all fifteen levels. Anything else that is
                // not an occluding shape lets the sky straight down and takes nothing away;
                // everything else costs the one level every cell costs a light entering it.
                const bool sky_down = permeable && !entry.value("can_occlude", true);
                info.dampening = static_cast<std::uint8_t>(
                    solid_render ? 15 : (sky_down ? 0 : 1));
                info.sky_down = sky_down;
                info.collides = entry.value("has_collision", true);

                const auto store = [&out, &info](std::int64_t id) {
                    if (id >= 0) {
                        out.block_light[static_cast<std::int32_t>(id)] = info;
                    }
                };
                if (entry.contains("states") && entry["states"].is_array()) {
                    for (const auto& state : entry["states"]) {
                        if (state.contains("id") && state["id"].is_number_integer()) {
                            store(state["id"].get<std::int64_t>());
                        }
                    }
                }
                else if (entry.contains("default_state") &&
                    entry["default_state"].contains("id")) {
                    store(entry["default_state"]["id"].get<std::int64_t>());
                }
            }
        }
        catch (const std::exception& e) {
            LOG_WARN(Data, "parse failed: {} ({})", path, e.what());
        }

        LOG_DEBUG(Data, "block light: {} states", out.block_light.size());
    }

} // namespace terila::data
