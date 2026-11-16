#pragma once

#include "version/mc_version.hpp"
#include "world/trf/trf_error.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace terila::world::trf {

    // §3: TRF metadata is little-endian. Chunk payloads are stored verbatim and never
    // byte-swapped, so the protocol's own big-endian encoding survives a save/load.
    inline constexpr std::uint16_t format_major = 1;
    inline constexpr std::uint16_t format_minor = 0;
    // Region payloads are stored verbatim, so the file is only readable by a build that
    // speaks the protocol the chunks were encoded for.
    inline constexpr std::uint16_t protocol_version =
        static_cast<std::uint16_t>(terila::version::protocol());

    inline constexpr std::size_t header_size = 4096;
    inline constexpr std::size_t footer_size = 4096;
    inline constexpr std::size_t slot_size = 64;
    inline constexpr std::size_t index_capacity = 1024;
    inline constexpr std::size_t index_entry_size = 128;
    inline constexpr std::size_t index_size = index_capacity * index_entry_size;
    inline constexpr std::size_t data_start = header_size + index_size;

    // Region files grow on demand instead of being created at the 4 GiB cap: extending a
    // file that the filesystem refuses to keep sparse costs minutes per region, and a
    // region that holds a few hundred chunks needs a few megabytes.
    inline constexpr std::uint64_t initial_region_size = 4ull * 1024 * 1024;
    inline constexpr std::uint64_t region_size = 0x1'0000'0000ull;
    inline constexpr std::uint64_t region_grow_quantum = 1024 * 1024;
    inline constexpr std::size_t data_align = 8;

    inline constexpr std::size_t slot_crc_offset = 0x1C;
    inline constexpr std::size_t slot_crc_coverage = slot_crc_offset;
    inline constexpr std::size_t slot_payload_offset = 0x20;

    inline constexpr std::size_t header_crc_offset = 0x06C;
    inline constexpr std::size_t header_crc_coverage = header_crc_offset;
    inline constexpr std::size_t header_sha256_offset = 0x070;
    inline constexpr std::size_t header_sha256_size = 32;
    inline constexpr std::size_t header_reserved_offset = 0x090;

    inline constexpr std::size_t footer_crc_offset = 0x048;
    inline constexpr std::size_t footer_crc_coverage = footer_crc_offset;
    inline constexpr std::size_t footer_sha256_offset = 0x028;

    inline constexpr std::size_t record_prefix_size = 8;
    inline constexpr std::uint32_t max_raw_size = 16u * 1024u * 1024u;
    inline constexpr std::uint32_t min_compressed_stored_size = 16;
    inline constexpr std::uint32_t stored_framing_overhead = 8;

    inline constexpr std::string_view magic = "TERILAR1";
    inline constexpr std::string_view footer_magic = "TERILAF1";

    inline constexpr std::array<std::byte, 8> magic_bytes = {
        std::byte{ 'T' }, std::byte{ 'E' }, std::byte{ 'R' }, std::byte{ 'I' },
        std::byte{ 'L' }, std::byte{ 'A' }, std::byte{ 'R' }, std::byte{ '1' },
    };

    inline constexpr std::array<std::byte, 8> footer_magic_bytes = {
        std::byte{ 'T' }, std::byte{ 'E' }, std::byte{ 'R' }, std::byte{ 'I' },
        std::byte{ 'L' }, std::byte{ 'A' }, std::byte{ 'F' }, std::byte{ '1' },
    };

    // §5 header flags.
    namespace header_flags {

        inline constexpr std::uint32_t sealed = 1u << 0;
        inline constexpr std::uint32_t generated = 1u << 1;
        inline constexpr std::uint32_t delta_mode = 1u << 2;
        inline constexpr std::uint32_t read_only = 1u << 3;
        inline constexpr std::uint32_t descriptor = 1u << 4;
        inline constexpr std::uint32_t reserved_mask = 0xFFFFFFE0u;

    }

    // §6 slot flags.
    namespace slot_flags {

        inline constexpr std::uint8_t edited = 1u << 0;
        inline constexpr std::uint8_t wire_ready = 1u << 1;
        inline constexpr std::uint8_t light_full = 1u << 2;
        inline constexpr std::uint8_t deleted = 1u << 3;
        inline constexpr std::uint8_t damaged = 1u << 4;
        inline constexpr std::uint8_t reserved_mask = 0xE0u;

    }

    // §15 codecs. 0 appears only in a canonical tombstone (§16).
    namespace compression {

        inline constexpr std::uint8_t none = 0;
        inline constexpr std::uint8_t stored = 1;
        inline constexpr std::uint8_t lz4 = 2;
        inline constexpr std::uint8_t zstd = 3;
        inline constexpr std::uint8_t first_codec = stored;
        inline constexpr std::uint8_t last_codec = zstd;

    }

    inline constexpr std::uint32_t no_chunk_id = 0xFFFFFFFFu;

    inline std::uint16_t load_u16(const std::byte* p) noexcept {
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
            (static_cast<std::uint16_t>(p[1]) << 8));
    }

    inline std::uint32_t load_u32(const std::byte* p) noexcept {
        return static_cast<std::uint32_t>(p[0]) |
            (static_cast<std::uint32_t>(p[1]) << 8) |
            (static_cast<std::uint32_t>(p[2]) << 16) |
            (static_cast<std::uint32_t>(p[3]) << 24);
    }

    inline std::uint64_t load_u64(const std::byte* p) noexcept {
        return static_cast<std::uint64_t>(load_u32(p)) |
            (static_cast<std::uint64_t>(load_u32(p + 4)) << 32);
    }

    inline std::int32_t load_i32(const std::byte* p) noexcept {
        return static_cast<std::int32_t>(load_u32(p));
    }

    inline std::int64_t load_i64(const std::byte* p) noexcept {
        return static_cast<std::int64_t>(load_u64(p));
    }

    inline void store_u16(std::byte* p, std::uint16_t v) noexcept {
        p[0] = static_cast<std::byte>(v & 0xFFu);
        p[1] = static_cast<std::byte>((v >> 8) & 0xFFu);
    }

    inline void store_u32(std::byte* p, std::uint32_t v) noexcept {
        p[0] = static_cast<std::byte>(v & 0xFFu);
        p[1] = static_cast<std::byte>((v >> 8) & 0xFFu);
        p[2] = static_cast<std::byte>((v >> 16) & 0xFFu);
        p[3] = static_cast<std::byte>((v >> 24) & 0xFFu);
    }

    inline void store_u64(std::byte* p, std::uint64_t v) noexcept {
        store_u32(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
        store_u32(p + 4, static_cast<std::uint32_t>(v >> 32));
    }

    inline void store_i32(std::byte* p, std::int32_t v) noexcept {
        store_u32(p, static_cast<std::uint32_t>(v));
    }

    inline void store_i64(std::byte* p, std::int64_t v) noexcept {
        store_u64(p, static_cast<std::uint64_t>(v));
    }

    inline std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) noexcept {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    inline bool is_zero(std::span<const std::byte> bytes) noexcept {
        for (const std::byte b : bytes) {
            if (b != std::byte{ 0 }) {
                return false;
            }
        }
        return true;
    }

    // §4: chunk id inside a region; lx/lz are the low five bits of the chunk coordinates.
    inline std::uint32_t chunk_id(std::int32_t cx, std::int32_t cz) noexcept {
        return static_cast<std::uint32_t>((cz & 31) * 32 + (cx & 31));
    }

    // Region coordinates are block coordinates / 512, i.e. chunk coordinates / 32, and
    // they must floor toward negative infinity rather than toward zero.
    inline std::int32_t region_coordinate(std::int32_t chunk_coordinate) noexcept {
        return chunk_coordinate >= 0 ? chunk_coordinate / 32 : -(((-chunk_coordinate) + 31) / 32);
    }

    inline std::uint64_t now_ms() noexcept {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

}
