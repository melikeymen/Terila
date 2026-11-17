#pragma once

#include "world/trf/trf_error.hpp"
#include "world/trf/trf_format.hpp"
#include "world/trf/trf_region.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace terila::world::trf {

    // Identity every region of one world carries, so a region file copied in from another
    // world is rejected instead of silently merged (§5, §5.1).
    struct WorldIdentity {
        std::uint64_t seed = 0;   // the seed the world was created with
        std::array<std::byte, 16> world_uuid{};
        std::uint32_t generator_id = 0;
        std::uint16_t generator_version = 1;
    };

    // Reads world/world.trfmeta, creating it with a fresh identity on first run. A
    // non-zero seed is stored in that new descriptor; zero draws a random one, so a world
    // keeps its seed across restarts while every new world starts somewhere else.
    std::expected<WorldIdentity, TrfError> load_or_create_world(
        const std::filesystem::path& world_root, std::uint32_t seed = 0);

    // §4 directory layout: world/dimensions/<namespace>/<path>/region/r.<cx>.<cz>.trf.
    std::filesystem::path default_region_directory(const std::filesystem::path& world_root,
        const std::string& dimension = "overworld");

    // One open file per region, bounded by an LRU budget (§18).
    class ChunkStore {
    public:
        ChunkStore(std::filesystem::path region_directory, WorldIdentity identity);

        std::expected<LoadResult, TrfError> load_chunk(std::int32_t cx, std::int32_t cz,
            std::vector<std::byte>& out);

        std::expected<void, TrfError> save_chunk(std::int32_t cx, std::int32_t cz,
            std::span<const std::byte> payload, std::uint8_t flags);

        std::expected<bool, TrfError> delete_chunk(std::int32_t cx, std::int32_t cz);

        std::expected<void, TrfError> flush();

        const std::filesystem::path& region_directory() const noexcept { return directory_; }

    private:
        std::expected<TrfRegion*, TrfError> region_for(std::int32_t cx, std::int32_t cz);
        std::filesystem::path region_path(std::int32_t rx, std::int32_t rz) const;

        struct CachedRegion {
            std::unique_ptr<TrfRegion> region;
            std::uint64_t last_used = 0;
        };

        std::filesystem::path directory_;
        WorldIdentity identity_;
        std::unordered_map<std::uint64_t, CachedRegion> regions_;
        std::uint64_t access_clock_ = 0;
    };

    std::uint8_t chunk_flags(bool edited, bool wire_ready, bool light_full);

}
