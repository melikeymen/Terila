#pragma once

#include "world/trf/trf_error.hpp"
#include "world/trf/trf_file.hpp"
#include "world/trf/trf_format.hpp"
#include "world/trf/trf_header.hpp"
#include "world/trf/trf_slot.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace terila::world::trf {

    struct RegionOptions {
        bool create_if_missing = true;
        std::int32_t region_x = 0;
        std::int32_t region_z = 0;
        std::uint64_t seed = 0;
        std::array<std::byte, 16> world_uuid{};
        std::uint32_t generator_id = 0;
        std::uint16_t generator_version = 0;
        std::uint32_t default_compression = compression::stored;
        std::uint32_t extra_flags = 0;
    };

    // §13: three outcomes, distinguished because absence and deletion mean different
    // things to the world layer.
    struct LoadResult {
        bool present = false;
        bool tombstone = false;
        std::uint32_t generation = 0;
        bool wire_ready = false;
        bool edited = false;
        // The stored light was computed by the current light rules. Saves from before the
        // rules knew about light sources carry false here, which is what tells the loader to
        // light the column again instead of streaming a copy that is dark around a lamp.
        bool light_full = false;
        std::uint8_t compression = 0;
    };

    class TrfRegion {
    public:
        static std::expected<std::unique_ptr<TrfRegion>, TrfError> open(
            const std::filesystem::path& path, const RegionOptions& options);

        ~TrfRegion();

        TrfRegion(const TrfRegion&) = delete;
        TrfRegion& operator=(const TrfRegion&) = delete;

        std::expected<LoadResult, TrfError> load_chunk(std::int32_t cx, std::int32_t cz,
            std::vector<std::byte>& out);

        std::expected<void, TrfError> save_chunk(std::int32_t cx, std::int32_t cz,
            std::span<const std::byte> payload, std::uint8_t flags);

        std::expected<bool, TrfError> delete_chunk(std::int32_t cx, std::int32_t cz);

        std::expected<void, TrfError> compact();

        std::expected<void, TrfError> seal();

        std::expected<void, TrfError> flush();

        void close() noexcept;

        const Header& header() const noexcept { return header_; }
        const std::filesystem::path& path() const noexcept { return path_; }
        bool sealed() const noexcept { return header_.sealed(); }

    private:
        TrfRegion() = default;

        static std::expected<void, TrfError> initialise(const std::filesystem::path& path,
            const RegionOptions& options);

        std::expected<void, TrfError> read_metadata(const RegionOptions& options);
        std::expected<void, TrfError> rebuild_counters(std::uint64_t file_size);

        std::expected<void, TrfError> write_header();
        std::expected<void, TrfError> write_footer(bool barrier);
        std::expected<void, TrfError> write_index();
        std::expected<void, TrfError> unseal();

        std::optional<Slot> winner(std::uint32_t id) const noexcept;
        int shadow_slot(std::uint32_t id) const noexcept;

        std::expected<void, TrfError> append_record(std::uint32_t id, std::span<const std::byte> payload,
            std::uint8_t flags);

        // Grows the file (never past the 4 GiB cap) so the record fits before the footer.
        std::expected<void, TrfError> ensure_capacity(std::uint64_t needed_end);

        // compact() takes the lock; the save path already holds it and calls this instead
        // (§16 step 1 forbids re-locking the same mutex).
        std::expected<void, TrfError> compact_locked();

        std::vector<std::byte> build_index_bytes() const;

        std::filesystem::path path_;
        TrfFile file_;
        Header header_{};
        std::uint64_t file_size_ = 0;
        bool header_dirty_ = false;
        bool footer_dirty_ = false;
        bool read_only_ = false;
        std::array<std::array<std::optional<Slot>, 2>, index_capacity> slots_{};
        mutable std::mutex mutex_;
    };

}
