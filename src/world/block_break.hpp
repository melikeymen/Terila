#pragma once

// How long a block resists a bare hand.
//
// The game describes a block's resistance as "hardness", in seconds of work: a block that
// needs no particular tool falls in hardness × 1.5 seconds, one that does need a tool takes
// five times as long without it. Negative hardness is bedrock and friends, which never yield.
// Tools are not modelled yet, so every dig here is the bare-hand case.

#include <cmath>

namespace terila::world {

    // Seconds of work per point of hardness: 1.5 s when the block yields to a bare hand,
    // 5 s when it wants a tool that is not being used.
    inline constexpr double kSoftSecondsPerHardness = 1.5;
    inline constexpr double kToolSecondsPerHardness = 5.0;
    inline constexpr double kTicksPerSecond = 20.0;

    // Ticks until the block gives way; -1 when it never does.
    inline int break_ticks(float hardness, bool requires_correct_tool) noexcept {
        if (!(hardness >= 0.0f)) {   // also catches NaN
            return -1;
        }
        const double per_hardness = requires_correct_tool ? kToolSecondsPerHardness
            : kSoftSecondsPerHardness;
        const double ticks = static_cast<double>(hardness) * per_hardness * kTicksPerSecond;
        return static_cast<int>(std::ceil(ticks));
    }

} // namespace terila::world
