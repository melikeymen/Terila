#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace terila::world::trf {

    using Sha256Digest = std::array<std::byte, 32>;

    // §9: the sealed digest covers metadata only and is computed at seal time, never on
    // the commit path.
    //
    // Empty when OpenSSL cannot provide the digest: writing the all-zero fallback would seal
    // a file whose header can never be verified again.
    std::optional<Sha256Digest> sha256(std::span<const std::byte> data) noexcept;

}
