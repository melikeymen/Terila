#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace terila::world::trf {

    using Sha256Digest = std::array<std::byte, 32>;

    // §9: the sealed digest covers metadata only and is computed at seal time, never on
    // the commit path.
    Sha256Digest sha256(std::span<const std::byte> data) noexcept;

}
