#include "world/trf/trf_crc32c.hpp"

#include <array>
#include <cstring>

#if (defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__))
#define TERILA_TRF_CRC32C_X86 1
#include <nmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#define TERILA_TRF_CRC32C_X86 0
#endif

// GCC and Clang only accept the CRC32 intrinsics inside a function compiled for
// the feature, even when the call is guarded by a runtime CPU check.
#if TERILA_TRF_CRC32C_X86 && !defined(_MSC_VER)
#define TERILA_TRF_SSE42 __attribute__((target("sse4.2")))
#else
#define TERILA_TRF_SSE42
#endif

#if defined(__ARM_FEATURE_CRC32)
#define TERILA_TRF_CRC32C_ARM 1
#include <arm_acle.h>
#else
#define TERILA_TRF_CRC32C_ARM 0
#endif

namespace terila::world::trf {

    namespace {

        constexpr std::array<std::uint32_t, 256> make_table() noexcept {
            std::array<std::uint32_t, 256> table{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int bit = 0; bit < 8; ++bit) {
                    c = (c & 1u) ? ((c >> 1) ^ crc32c_polynomial) : (c >> 1);
                }
                table[i] = c;
            }
            return table;
        }

        constexpr std::array<std::uint32_t, 256> table = make_table();

        bool detect_hardware() noexcept {
#if TERILA_TRF_CRC32C_X86
#if defined(_MSC_VER)
            int regs[4] = { 0, 0, 0, 0 };
            __cpuid(regs, 1);
            return (regs[2] & (1 << 20)) != 0;
#elif defined(__GNUC__)
            return __builtin_cpu_supports("sse4.2");
#else
            return false;
#endif
#elif TERILA_TRF_CRC32C_ARM
            return true;
#else
            return false;
#endif
        }

        const bool hardware_available = detect_hardware();

    }

    std::uint32_t crc32c_software(std::span<const std::byte> data, std::uint32_t seed) noexcept {
        std::uint32_t crc = ~seed;
        for (const std::byte b : data) {
            const auto index = static_cast<std::uint8_t>(crc) ^ static_cast<std::uint8_t>(b);
            crc = table[index] ^ (crc >> 8);
        }
        return ~crc;
    }

#if TERILA_TRF_CRC32C_X86
    TERILA_TRF_SSE42 std::uint32_t crc32c_x86(std::span<const std::byte> data,
        std::uint32_t seed) noexcept {
        std::uint32_t crc = ~seed;
        const std::byte* p = data.data();
        std::size_t remaining = data.size();

#if defined(_M_X64) || defined(__x86_64__)
        while (remaining >= 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, p, 8);
            crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, word));
            p += 8;
            remaining -= 8;
        }
#else
        while (remaining >= 4) {
            std::uint32_t word = 0;
            std::memcpy(&word, p, 4);
            crc = _mm_crc32_u32(crc, word);
            p += 4;
            remaining -= 4;
        }
#endif
        while (remaining > 0) {
            crc = _mm_crc32_u8(crc, static_cast<std::uint8_t>(*p));
            ++p;
            --remaining;
        }
        return ~crc;
    }
#endif

    std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed) noexcept {
        if (!hardware_available) {
            return crc32c_software(data, seed);
        }

#if TERILA_TRF_CRC32C_X86
        return crc32c_x86(data, seed);
#elif TERILA_TRF_CRC32C_ARM
        std::uint32_t crc = ~seed;
        const std::byte* p = data.data();
        std::size_t remaining = data.size();

        while (remaining >= 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, p, 8);
            crc = __crc32cd(crc, word);
            p += 8;
            remaining -= 8;
        }
        while (remaining > 0) {
            crc = __crc32cb(crc, static_cast<std::uint8_t>(*p));
            ++p;
            --remaining;
        }
        return ~crc;
#else
        return crc32c_software(data, seed);
#endif
    }

    bool crc32c_uses_hardware() noexcept {
        return hardware_available;
    }

}
