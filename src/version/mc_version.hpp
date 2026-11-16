#pragma once

// Which Minecraft protocol this build speaks.
//
// The tree keeps the supported releases side by side and compiles exactly one of them: CMake
// turns -DTERILA_MC_TARGET=26.3 into TERILA_MC_TARGET and everything else follows from here.
// 26.3 is the floor. Anything older is a build error instead of a runtime fallback, so a
// stale target cannot quietly produce packets the client will not understand.
//
// A release is added in two places: one row in the table below, and one block in
// features.hpp that lists only what changed. Behaviour that already works is inherited,
// never repeated.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// 26.3 -> 2603. Two digits per component keep the value ordered, so "at least" is an integer
// comparison rather than a chain of string tests. Minor versions are expected to stay below
// 100 (they are release numbers, not build numbers).
#define TERILA_MC_CODE(major, minor) (((major) * 100) + (minor))

#define TERILA_MC_FLOOR TERILA_MC_CODE(26, 3)

#ifndef TERILA_MC_TARGET
#define TERILA_MC_TARGET TERILA_MC_CODE(26, 3)
#endif

#if TERILA_MC_TARGET < TERILA_MC_FLOOR
#error "Terila speaks 26.3 and later only: TERILA_MC_TARGET is below TERILA_MC_CODE(26, 3)."
#endif

#define TERILA_MC_AT_LEAST(major, minor) (TERILA_MC_TARGET >= TERILA_MC_CODE(major, minor))
#define TERILA_MC_IS(major, minor) (TERILA_MC_TARGET == TERILA_MC_CODE(major, minor))

namespace terila::version {

    struct Protocol {
        int code = 0;                     // TERILA_MC_CODE form, compared, never printed
        std::string_view name{};          // "26.3", exactly as clients and packs report it
        std::int32_t number = 0;          // handshake protocol, 777 in 26.3
        std::int32_t data_version = 0;    // level format version, 5023 in 26.3
        std::string_view known_packs{};   // "core" version in select_known_packs
    };

    // One row per buildable release, oldest first. A new release appends a row; nothing above
    // it is edited.
    inline constexpr Protocol table[] = {
        {TERILA_MC_CODE(26, 3), "26.3", 777, 5023, "26.3"},
    };

    inline constexpr std::size_t row_count = sizeof(table) / sizeof(table[0]);

    namespace detail {

        inline constexpr std::size_t row_index(int code) noexcept {
            for (std::size_t i = 0; i < row_count; ++i) {
                if (table[i].code == code) {
                    return i;
                }
            }
            return row_count;   // no row: the static_assert below reports it
        }

    }

    inline constexpr std::size_t target_index = detail::row_index(TERILA_MC_TARGET);

    static_assert(target_index < row_count,
        "TERILA_MC_TARGET has no row in terila::version::table: add one before building for it.");

    inline constexpr const Protocol& target = table[target_index < row_count ? target_index : 0];

    inline constexpr int code() noexcept { return target.code; }
    inline constexpr std::string_view name() noexcept { return target.name; }
    inline constexpr std::int32_t protocol() noexcept { return target.number; }
    inline constexpr std::int32_t data_version() noexcept { return target.data_version; }
    inline constexpr std::string_view known_packs() noexcept { return target.known_packs; }

    // "26.3 (protocol 777)": one string for logs, so no call site repeats the numbers.
    inline std::string label() {
        return std::string(target.name) + " (protocol " + std::to_string(target.number) + ")";
    }

    // True when the extractor dump in input/ belongs to this protocol. A negative number
    // means the dump did not say, which is treated as "cannot tell" rather than a mismatch.
    inline bool matches_source(std::int32_t source_protocol) noexcept {
        return source_protocol < 0 || source_protocol == target.number;
    }

} // namespace terila::version
