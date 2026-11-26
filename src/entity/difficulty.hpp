#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace terila::entity {

    // The world's difficulty, in the order the protocol numbers it.
    enum class Difficulty : std::uint8_t {
        Peaceful = 0,
        Easy = 1,
        Normal = 2,
        Hard = 3
    };

    inline std::optional<Difficulty> difficulty_from(std::string_view name) {
        if (name == "peaceful") return Difficulty::Peaceful;
        if (name == "easy") return Difficulty::Easy;
        if (name == "normal") return Difficulty::Normal;
        if (name == "hard") return Difficulty::Hard;
        return std::nullopt;
    }

    inline const char* difficulty_name(Difficulty difficulty) noexcept {
        switch (difficulty) {
        case Difficulty::Peaceful: return "peaceful";
        case Difficulty::Easy: return "easy";
        case Difficulty::Normal: return "normal";
        case Difficulty::Hard: return "hard";
        }
        return "normal";
    }

    // The effective difficulty the scale is read against: a world that has run long enough
    // and is hard enough to stand at the top of it.
    inline constexpr float kFullPressure = 4.0f;

    // How dangerous a position has become. The base difficulty is the world setting; the
    // effective value also grows with the time the world has run, with the time this part of
    // it has been inhabited, and with the moon. It is what decides whether a freshly spawned
    // mob carries equipment, and it is why a world gets harder the longer it is played.
    struct ThreatLevel {
        Difficulty base = Difficulty::Normal;
        float effective = 0.0f;

        bool hard() const noexcept { return effective >= 3.0f; }

        bool harder_than(float requirement) const noexcept {
            return effective > requirement;
        }

        // The share of a chunk's monster spawn budget the world has earned: none while the
        // world is peaceful, a quarter at the bottom of the scale, all of it once the scale
        // is full. Vanilla spends the same budget at every difficulty; here the threat level
        // is what a world of that age and that moon is allowed to place.
        float monster_share() const noexcept {
            if (base == Difficulty::Peaceful) {
                return 0.0f;
            }
            return std::clamp(effective / kFullPressure, 0.25f, 1.0f);
        }

        // 0 below normal, 1 at the top of hard, linear in between.
        float special_multiplier() const noexcept {
            if (effective < 2.0f) {
                return 0.0f;
            }
            if (effective > 4.0f) {
                return 1.0f;
            }
            return (effective - 2.0f) / 2.0f;
        }
    };

    inline ThreatLevel threat_level(Difficulty base, std::int64_t total_game_time,
        std::int64_t local_ticks, float moon_brightness) {
        ThreatLevel level;
        level.base = base;
        if (base == Difficulty::Peaceful) {
            level.effective = 0.0f;
            return level;
        }

        const bool hard = base == Difficulty::Hard;
        const float global = std::clamp(
            (static_cast<float>(total_game_time) - 72000.0f) / 1440000.0f, 0.0f, 1.0f) * 0.25f;
        float local = std::clamp(static_cast<float>(local_ticks) / 3600000.0f, 0.0f, 1.0f) *
            (hard ? 1.0f : 0.75f);
        local += std::clamp(moon_brightness * 0.25f, 0.0f, global);
        if (base == Difficulty::Easy) {
            local *= 0.5f;
        }

        const float scale = 0.75f + global + local;
        level.effective = static_cast<float>(base) * scale;
        return level;
    }

} // namespace terila::entity
