#include "world/trf/trf_store.hpp"

#include "world/trf/trf_crc32c.hpp"
#include "world/trf/trf_header.hpp"
#include "world/trf/trf_sha256.hpp"

#include <cstring>
#include <fstream>
#include <random>
#include <string>

namespace terila::world::trf {

    namespace {

        constexpr std::size_t max_open_regions = 64;

        // A world identity has to differ between worlds: two worlds sharing a UUID would
        // accept each other's region files instead of reporting a mismatch (§10). A digest of
        // a constant gives every world on every machine the same value, so the bytes are
        // random and shaped like a V4 UUID.
        std::array<std::byte, 16> random_uuid() {
            std::array<std::byte, 16> uuid{};

            std::random_device device;
            const std::uint64_t mixed = (static_cast<std::uint64_t>(device()) << 32) ^
                static_cast<std::uint64_t>(device()) ^ now_ms();
            std::mt19937_64 engine(mixed);

            for (std::size_t i = 0; i < uuid.size(); i += 8) {
                const auto word = engine();
                for (std::size_t b = 0; b < 8 && i + b < uuid.size(); ++b) {
                    uuid[i + b] = static_cast<std::byte>((word >> (b * 8)) & 0xFFu);
                }
            }

            // RFC 4122 variant and version bits, so the value is a well-formed V4 id.
            uuid[6] = static_cast<std::byte>((static_cast<std::uint8_t>(uuid[6]) & 0x0F) | 0x40);
            uuid[8] = static_cast<std::byte>((static_cast<std::uint8_t>(uuid[8]) & 0x3F) | 0x80);
            return uuid;
        }

        // The seed a world gets when it did not name one. Drawing from the device keeps two
        // freshly created worlds apart; the time term keeps a fast double create apart too.
        std::uint32_t draw_seed() {
            std::random_device device;
            const std::uint64_t mixed = (static_cast<std::uint64_t>(device()) << 32) ^
                static_cast<std::uint64_t>(device()) ^ now_ms();
            const auto value = static_cast<std::uint32_t>(mixed ^ (mixed >> 32));
            return value != 0 ? value : 1u;
        }

        std::expected<WorldIdentity, TrfError> write_descriptor(const std::filesystem::path& path,
            const WorldIdentity& identity) {

            Header header;
            header.flags = header_flags::descriptor;
            header.seed = static_cast<std::int64_t>(identity.seed);
            header.world_uuid = identity.world_uuid;
            header.generator_id = identity.generator_id;
            header.generator_version = identity.generator_version;
            header.created_ms = now_ms();
            header.modified_ms = header.created_ms;
            header.data_end = static_cast<std::uint32_t>(data_start);

            std::array<std::byte, header_size> bytes{};
            header.encode(bytes);

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                return std::unexpected(make_error(TrfErrorCode::Io,
                    "cannot create world descriptor: " + path.string()));
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!out) {
                return std::unexpected(make_error(TrfErrorCode::Io,
                    "cannot write world descriptor: " + path.string()));
            }
            return identity;
        }

        std::expected<WorldIdentity, TrfError> read_descriptor(const std::filesystem::path& path) {
            std::array<std::byte, header_size> bytes{};
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return std::unexpected(make_error(TrfErrorCode::Io,
                    "cannot read world descriptor: " + path.string()));
            }
            in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
            if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
                return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                    "world descriptor is truncated"));
            }

            if (std::memcmp(bytes.data(), magic_bytes.data(), magic_bytes.size()) != 0) {
                return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                    "world descriptor has bad magic"));
            }
            if (crc32c(std::span<const std::byte>(bytes).first(header_crc_coverage)) !=
                load_u32(bytes.data() + header_crc_offset)) {
                return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                    "world descriptor CRC mismatch"));
            }
            if ((load_u32(bytes.data() + 0x010) & header_flags::descriptor) == 0) {
                return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                    "world descriptor is missing the descriptor flag"));
            }
            if (load_u16(bytes.data() + 0x008) != format_major) {
                return std::unexpected(make_error(TrfErrorCode::UnsupportedFormat,
                    "world descriptor uses an unsupported format major"));
            }

            WorldIdentity identity;
            identity.seed = static_cast<std::uint64_t>(load_i64(bytes.data() + 0x048));
            std::memcpy(identity.world_uuid.data(), bytes.data() + 0x050, identity.world_uuid.size());
            identity.generator_version = load_u16(bytes.data() + 0x062);
            identity.generator_id = load_u32(bytes.data() + 0x064);
            return identity;
        }

    }

    std::expected<WorldIdentity, TrfError> load_or_create_world(
        const std::filesystem::path& world_root, std::optional<std::uint32_t> seed) {
        std::error_code ec;
        std::filesystem::create_directories(world_root, ec);
        if (ec) {
            return std::unexpected(make_error(TrfErrorCode::Io,
                "cannot create world directory: " + world_root.string()));
        }

        const auto path = world_root / "world.trfmeta";
        if (std::filesystem::exists(path, ec)) {
            return read_descriptor(path);
        }

        WorldIdentity identity;
        identity.world_uuid = random_uuid();
        // A world that names no seed gets a random one, drawn once and then kept.
        identity.seed = seed.value_or(draw_seed());
        identity.generator_id = static_cast<std::uint32_t>(crc32c(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>("terila.noise.v2"), 15)));
        identity.generator_version = 3;
        return write_descriptor(path, identity);
    }

    std::filesystem::path default_region_directory(const std::filesystem::path& world_root,
        const std::string& dimension) {
        return world_root / "dimensions" / "minecraft" / dimension / "region";
    }

    std::uint8_t chunk_flags(bool edited, bool wire_ready, bool light_full) {
        std::uint8_t flags = 0;
        if (edited) {
            flags = static_cast<std::uint8_t>(flags | slot_flags::edited);
        }
        if (wire_ready) {
            flags = static_cast<std::uint8_t>(flags | slot_flags::wire_ready);
        }
        if (light_full) {
            flags = static_cast<std::uint8_t>(flags | slot_flags::light_full);
        }
        return flags;
    }

    ChunkStore::ChunkStore(std::filesystem::path region_directory, WorldIdentity identity)
        : directory_(std::move(region_directory)), identity_(identity) {
        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
    }

    std::filesystem::path ChunkStore::region_path(std::int32_t rx, std::int32_t rz) const {
        return directory_ / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".trf");
    }

    std::expected<std::shared_ptr<TrfRegion>, TrfError> ChunkStore::region_for(
        std::int32_t cx, std::int32_t cz) {

        const std::int32_t rx = region_coordinate(cx);
        const std::int32_t rz = region_coordinate(cz);
        const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rx)) << 32) |
            static_cast<std::uint32_t>(rz);

        
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            const auto found = regions_.find(key);
            if (found != regions_.end()) {
                found->second.last_used = ++access_clock_;
                return found->second.region;  // shared_ptr copy
            }
        }

        // Cache miss
        RegionOptions options;
        options.create_if_missing = true;
        options.region_x = rx;
        options.region_z = rz;
        options.seed = identity_.seed;
        options.world_uuid = identity_.world_uuid;
        options.generator_id = identity_.generator_id;
        options.generator_version = identity_.generator_version;
        options.default_compression = compression::lz4;

        auto opened = TrfRegion::open(region_path(rx, rz), options);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        std::shared_ptr<TrfRegion> region = std::move(*opened);

        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto existing = regions_.find(key);
        if (existing != regions_.end()) {
            existing->second.last_used = ++access_clock_;
            return existing->second.region;
        }

        if (regions_.size() >= max_open_regions) {
            auto oldest = regions_.end();
            for (auto it = regions_.begin(); it != regions_.end(); ++it) {
                if (oldest == regions_.end() || it->second.last_used < oldest->second.last_used) {
                    oldest = it;
                }
            }
            regions_.erase(oldest);
        }

        auto& cached = regions_[key];
        cached.region = region;
        cached.last_used = ++access_clock_;
        return region;
    }

    std::expected<LoadResult, TrfError> ChunkStore::load_chunk(std::int32_t cx, std::int32_t cz,
        std::vector<std::byte>& out) {

        auto region = region_for(cx, cz);
        if (!region) {
            return std::unexpected(region.error());
        }
        return (*region)->load_chunk(cx, cz, out);
    }

    std::expected<void, TrfError> ChunkStore::save_chunk(std::int32_t cx, std::int32_t cz,
        std::span<const std::byte> payload, std::uint8_t flags) {

        auto region = region_for(cx, cz);
        if (!region) {
            return std::unexpected(region.error());
        }
        return (*region)->save_chunk(cx, cz, payload, flags);
    }

    std::expected<bool, TrfError> ChunkStore::delete_chunk(std::int32_t cx, std::int32_t cz) {
        auto region = region_for(cx, cz);
        if (!region) {
            return std::unexpected(region.error());
        }
        return (*region)->delete_chunk(cx, cz);
    }

    std::expected<void, TrfError> ChunkStore::flush() {
        std::vector<std::shared_ptr<TrfRegion>> snapshot;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            snapshot.reserve(regions_.size());
            for (auto& entry : regions_) {
                snapshot.push_back(entry.second.region);
            }
        }
        for (auto& region : snapshot) {
            if (auto flushed = region->flush(); !flushed) {
                return std::unexpected(flushed.error());
            }
        }
        return {};
    }

}
