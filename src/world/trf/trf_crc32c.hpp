#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace terila::world::trf {

    // Castagnoli polynomial, reflected: 0x82F63B78 (§9).
    inline constexpr std::uint32_t crc32c_polynomial = 0x82F63B78u;

    std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed = 0) noexcept;
    std::uint32_t crc32c_software(std::span<const std::byte> data, std::uint32_t seed = 0) noexcept;
    bool crc32c_uses_hardware() noexcept;

}
