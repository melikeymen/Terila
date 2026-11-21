#pragma once

// Which dimension a coordinate belongs to. It lives apart from the generator's view of a
// dimension so the block overlay and the network layer can name one without pulling the
// worldgen data readers into their headers.

#include <cstdint>

namespace terila::world {

    enum class DimensionKind : std::uint8_t {
        Overworld = 0,
        Nether = 1,
        End = 2,
    };

    inline constexpr int kDimensionCount = 3;

    inline constexpr int dimension_index(DimensionKind kind) noexcept {
        return static_cast<int>(kind);
    }

} // namespace terila::world
