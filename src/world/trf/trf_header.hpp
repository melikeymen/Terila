#pragma once

#include "world/trf/trf_error.hpp"
#include "world/trf/trf_format.hpp"
#include "world/trf/trf_sha256.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace terila::world::trf {

    // §5 header, laid out at absolute file offset 0.
    struct Header {
        std::uint16_t version_major = format_major;
        std::uint16_t version_minor = format_minor;
        std::uint32_t flags = 0;
        std::int32_t region_x = 0;
        std::int32_t region_z = 0;
        std::uint32_t chunk_count = 0;
        std::uint32_t index_capacity = static_cast<std::uint32_t>(trf::index_capacity);
        std::uint32_t index_entry_size = static_cast<std::uint32_t>(trf::index_entry_size);
        std::uint32_t data_end = static_cast<std::uint32_t>(data_start);
        std::uint32_t dead_bytes = 0;
        std::uint64_t generation = 0;
        std::uint64_t created_ms = 0;
        std::uint64_t modified_ms = 0;
        std::int64_t seed = 0;
        std::array<std::byte, 16> world_uuid{};
        std::uint16_t protocol_version = trf::protocol_version;
        std::uint16_t generator_version = 0;
        std::uint32_t generator_id = 0;
        std::uint32_t default_compression = compression::stored;
        Sha256Digest sha256{};

        bool sealed() const noexcept { return (flags & header_flags::sealed) != 0; }
        bool delta_mode() const noexcept { return (flags & header_flags::delta_mode) != 0; }

        void encode(std::span<std::byte, header_size> out) const noexcept;

        static std::expected<Header, TrfError> decode(std::span<const std::byte, header_size> in);
    };

    // §7 footer, at file_size - 4096.
    struct Footer {
        std::uint16_t version_major = format_major;
        std::uint16_t version_minor = format_minor;
        std::uint32_t chunk_count = 0;
        std::uint32_t data_end = static_cast<std::uint32_t>(data_start);
        std::uint64_t generation = 0;
        std::int32_t region_x = 0;
        std::int32_t region_z = 0;
        std::uint32_t index_crc32c = 0;
        Sha256Digest sha256{};

        void encode(std::span<std::byte, footer_size> out) const noexcept;

        static std::optional<Footer> decode(std::span<const std::byte, footer_size> in) noexcept;

        static Footer from_header(const Header& header);
    };

}
