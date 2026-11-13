#include "world/trf/trf_header.hpp"

#include "world/trf/trf_crc32c.hpp"

#include <algorithm>
#include <cstring>

namespace terila::world::trf {

    namespace {

        std::string describe_version(std::uint16_t major, std::uint16_t minor) {
            return "format " + std::to_string(major) + "." + std::to_string(minor);
        }

    }

    void Header::encode(std::span<std::byte, header_size> out) const noexcept {
        std::fill(out.begin(), out.end(), std::byte{ 0 });

        std::memcpy(out.data() + 0x000, magic_bytes.data(), magic_bytes.size());
        store_u16(out.data() + 0x008, version_major);
        store_u16(out.data() + 0x00A, version_minor);
        store_u32(out.data() + 0x00C, static_cast<std::uint32_t>(header_size));
        store_u32(out.data() + 0x010, flags);
        store_i32(out.data() + 0x014, region_x);
        store_i32(out.data() + 0x018, region_z);
        store_u32(out.data() + 0x01C, chunk_count);
        store_u32(out.data() + 0x020, index_capacity);
        store_u32(out.data() + 0x024, index_entry_size);
        store_u32(out.data() + 0x028, data_end);
        store_u32(out.data() + 0x02C, dead_bytes);
        store_u64(out.data() + 0x030, generation);
        store_u64(out.data() + 0x038, created_ms);
        store_u64(out.data() + 0x040, modified_ms);
        store_i64(out.data() + 0x048, seed);
        std::memcpy(out.data() + 0x050, world_uuid.data(), world_uuid.size());
        store_u16(out.data() + 0x060, protocol_version);
        store_u16(out.data() + 0x062, generator_version);
        store_u32(out.data() + 0x064, generator_id);
        store_u32(out.data() + 0x068, default_compression);

        // The digest lives outside the CRC range, so writing it first keeps the CRC the
        // last field that changes inside [0x000, 0x06C) (§7.1 step 3).
        std::memcpy(out.data() + header_sha256_offset, sha256.data(), sha256.size());
        store_u32(out.data() + header_crc_offset, crc32c(out.first(header_crc_coverage)));
    }

    std::expected<Header, TrfError> Header::decode(std::span<const std::byte, header_size> in) {
        if (std::memcmp(in.data(), magic_bytes.data(), magic_bytes.size()) != 0) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader, "bad magic"));
        }

        Header header;
        header.version_major = load_u16(in.data() + 0x008);
        header.version_minor = load_u16(in.data() + 0x00A);
        if (header.version_major != format_major) {
            return std::unexpected(make_error(TrfErrorCode::UnsupportedFormat,
                describe_version(header.version_major, header.version_minor)));
        }

        if (load_u32(in.data() + 0x00C) != header_size) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader, "header_size is not 4096"));
        }
        if (load_u32(in.data() + 0x020) != trf::index_capacity) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader, "index_capacity is not 1024"));
        }
        if (load_u32(in.data() + 0x024) != trf::index_entry_size) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader, "index_entry_size is not 128"));
        }

        // Fields a future minor version may add live in the reserved tail; a reader accepts
        // them only while it can prove the file does not depend on them (§25).
        if (header.version_minor != format_minor &&
            !is_zero(std::span<const std::byte>(in).subspan(header_reserved_offset))) {
            return std::unexpected(make_error(TrfErrorCode::UnsupportedFormat,
                "unknown fields set for " +
                describe_version(header.version_major, header.version_minor)));
        }

        header.flags = load_u32(in.data() + 0x010);
        if ((header.flags & header_flags::descriptor) != 0) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                "descriptor flag is not valid in a region header"));
        }
        if ((header.flags & header_flags::reserved_mask) != 0) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                "header uses reserved flag bits"));
        }

        if (crc32c(in.first(header_crc_coverage)) != load_u32(in.data() + header_crc_offset)) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader, "header_crc32c mismatch"));
        }

        header.region_x = load_i32(in.data() + 0x014);
        header.region_z = load_i32(in.data() + 0x018);
        header.chunk_count = load_u32(in.data() + 0x01C);
        header.data_end = load_u32(in.data() + 0x028);
        header.dead_bytes = load_u32(in.data() + 0x02C);
        header.generation = load_u64(in.data() + 0x030);
        header.created_ms = load_u64(in.data() + 0x038);
        header.modified_ms = load_u64(in.data() + 0x040);
        header.seed = load_i64(in.data() + 0x048);
        std::memcpy(header.world_uuid.data(), in.data() + 0x050, header.world_uuid.size());
        header.protocol_version = load_u16(in.data() + 0x060);
        header.generator_version = load_u16(in.data() + 0x062);
        header.generator_id = load_u32(in.data() + 0x064);
        header.default_compression = load_u32(in.data() + 0x068);
        std::memcpy(header.sha256.data(), in.data() + header_sha256_offset, header.sha256.size());

        // Payloads are protocol-shaped, so a different protocol version cannot be
        // reinterpreted even though every metadata field parses (§5).
        if (header.protocol_version != trf::protocol_version) {
            return std::unexpected(make_error(TrfErrorCode::UnsupportedFormat,
                "protocol_version " + std::to_string(header.protocol_version) +
                " cannot be read as " + std::to_string(trf::protocol_version)));
        }
        if (header.default_compression < compression::first_codec ||
            header.default_compression > compression::last_codec) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                "default_compression is not a codec"));
        }

        // A digest is only meaningful on a sealed file (§7.1 verification rules).
        if (!header.sealed() &&
            !is_zero(std::span<const std::byte>(header.sha256.data(), header.sha256.size()))) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                "unsealed header carries a sha256 digest"));
        }

        return header;
    }

    void Footer::encode(std::span<std::byte, footer_size> out) const noexcept {
        std::fill(out.begin(), out.end(), std::byte{ 0 });

        std::memcpy(out.data() + 0x000, footer_magic_bytes.data(), footer_magic_bytes.size());
        store_u16(out.data() + 0x008, version_major);
        store_u16(out.data() + 0x00A, version_minor);
        store_u32(out.data() + 0x00C, chunk_count);
        store_u32(out.data() + 0x010, data_end);
        store_u64(out.data() + 0x014, generation);
        store_i32(out.data() + 0x01C, region_x);
        store_i32(out.data() + 0x020, region_z);
        store_u32(out.data() + 0x024, index_crc32c);
        std::memcpy(out.data() + footer_sha256_offset, sha256.data(), sha256.size());
        store_u32(out.data() + footer_crc_offset, crc32c(out.first(footer_crc_coverage)));
    }

    std::optional<Footer> Footer::decode(std::span<const std::byte, footer_size> in) noexcept {
        if (std::memcmp(in.data(), footer_magic_bytes.data(), footer_magic_bytes.size()) != 0) {
            return std::nullopt;
        }
        if (load_u16(in.data() + 0x008) != format_major) {
            return std::nullopt;
        }
        if (crc32c(in.first(footer_crc_coverage)) != load_u32(in.data() + footer_crc_offset)) {
            return std::nullopt;
        }

        Footer footer;
        footer.version_minor = load_u16(in.data() + 0x00A);
        footer.chunk_count = load_u32(in.data() + 0x00C);
        footer.data_end = load_u32(in.data() + 0x010);
        footer.generation = load_u64(in.data() + 0x014);
        footer.region_x = load_i32(in.data() + 0x01C);
        footer.region_z = load_i32(in.data() + 0x020);
        footer.index_crc32c = load_u32(in.data() + 0x024);
        std::memcpy(footer.sha256.data(), in.data() + footer_sha256_offset, footer.sha256.size());
        return footer;
    }

    Footer Footer::from_header(const Header& header) {
        Footer footer;
        footer.version_minor = header.version_minor;
        footer.chunk_count = header.chunk_count;
        footer.data_end = header.data_end;
        footer.generation = header.generation;
        footer.region_x = header.region_x;
        footer.region_z = header.region_z;
        footer.sha256 = header.sealed() ? header.sha256 : Sha256Digest{};
        return footer;
    }

}
