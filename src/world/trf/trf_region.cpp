#include "world/trf/trf_region.hpp"

#include "world/trf/trf_crc32c.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace terila::world::trf {

    namespace {

        std::uint64_t entry_offset(std::uint32_t id) noexcept {
            return header_size + static_cast<std::uint64_t>(id) * index_entry_size;
        }

        std::uint64_t slot_offset_of(std::uint32_t id, int slot) noexcept {
            return entry_offset(id) + static_cast<std::uint64_t>(slot) * slot_size;
        }

        TrfError io_error(std::string detail) {
            return make_error(TrfErrorCode::Io, std::move(detail));
        }

    }

    TrfRegion::~TrfRegion() {
        close();
    }

    void TrfRegion::close() noexcept {
        if (!file_.valid()) {
            return;
        }
        if (header_dirty_ || footer_dirty_) {
            write_footer(false);
            write_header();
        }
        file_.barrier();
        file_.close();
    }

    std::expected<void, TrfError> TrfRegion::initialise(const std::filesystem::path& path,
        const RegionOptions& options) {

        auto temporary = path;
        temporary += ".tmp";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);

        if (auto created = TrfFile::create(path, initial_region_size); !created) {
            return std::unexpected(created.error());
        }

        auto file = TrfFile::open(path);
        if (!file) {
            return std::unexpected(file.error());
        }

        Header header;
        header.region_x = options.region_x;
        header.region_z = options.region_z;
        header.seed = static_cast<std::int64_t>(options.seed);
        header.world_uuid = options.world_uuid;
        header.generator_id = options.generator_id;
        header.generator_version = options.generator_version;
        header.default_compression = options.default_compression;
        header.flags = options.extra_flags & ~header_flags::sealed;
        header.data_end = static_cast<std::uint32_t>(data_start);
        header.created_ms = now_ms();
        header.modified_ms = header.created_ms;

        std::array<std::byte, header_size> header_bytes{};
        header.encode(header_bytes);
        if (auto written = file->write_at(0, header_bytes); !written) {
            return std::unexpected(written.error());
        }

        Footer footer = Footer::from_header(header);
        std::array<std::byte, footer_size> footer_bytes{};
        footer.encode(footer_bytes);
        if (auto written = file->write_at(initial_region_size - footer_size, footer_bytes); !written) {
            return std::unexpected(written.error());
        }

        if (auto synced = file->barrier(); !synced) {
            return std::unexpected(synced.error());
        }
        file->close();

        return TrfFile::sync_directory(path.parent_path());
    }

    std::expected<std::unique_ptr<TrfRegion>, TrfError> TrfRegion::open(
        const std::filesystem::path& path, const RegionOptions& options) {

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            if (!options.create_if_missing) {
                return std::unexpected(make_error(TrfErrorCode::NotFound,
                    "region file does not exist: " + path.string()));
            }
            if (auto created = initialise(path, options); !created) {
                return std::unexpected(created.error());
            }
        }

        auto file = TrfFile::open(path);
        if (!file) {
            return std::unexpected(file.error());
        }
        if (auto locked = file->lock_exclusive(); !locked) {
            return std::unexpected(locked.error());
        }

        auto region = std::unique_ptr<TrfRegion>(new TrfRegion());
        region->path_ = path;
        region->file_ = std::move(*file);

        auto size = region->file_.size();
        if (!size) {
            return std::unexpected(size.error());
        }
        region->file_size_ = *size;

        if (auto metadata = region->read_metadata(options); !metadata) {
            return std::unexpected(metadata.error());
        }
        if (auto counters = region->rebuild_counters(region->file_size_); !counters) {
            return std::unexpected(counters.error());
        }
        if (region->header_dirty_ && !region->header_.sealed()) {
            if (auto written = region->write_header(); !written) {
                return std::unexpected(written.error());
            }
        }
        if (region->footer_dirty_) {
            if (auto written = region->write_footer(false); !written) {
                return std::unexpected(written.error());
            }
        }

        return region;
    }

    std::expected<void, TrfError> TrfRegion::read_metadata(const RegionOptions& options) {
        if (file_size_ < header_size + footer_size) {
            return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                "file is smaller than one header plus one footer"));
        }

        auto temporary = path_;
        temporary += ".tmp";
        if (std::filesystem::exists(temporary)) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }

        std::array<std::byte, header_size> header_bytes{};
        if (auto read = file_.read_at(0, header_bytes); !read) {
            return std::unexpected(read.error());
        }

        std::optional<Footer> footer;
        std::array<std::byte, footer_size> footer_bytes{};
        if (auto read = file_.read_at(file_size_ - footer_size, footer_bytes); read) {
            footer = Footer::decode(footer_bytes);
        }

        auto decoded = Header::decode(header_bytes);
        if (!decoded) {
            // A torn header is rebuilt from a valid footer (§12 step 3): the digest it
            // carried described a header that no longer exists, so SEALED is dropped.
            if (!footer || footer->region_x != options.region_x ||
                footer->region_z != options.region_z) {
                return std::unexpected(decoded.error());
            }

            Header rebuilt;
            rebuilt.region_x = options.region_x;
            rebuilt.region_z = options.region_z;
            rebuilt.seed = static_cast<std::int64_t>(options.seed);
            rebuilt.world_uuid = options.world_uuid;
            rebuilt.generator_id = options.generator_id;
            rebuilt.generator_version = options.generator_version;
            rebuilt.default_compression = options.default_compression;
            rebuilt.generation = footer->generation;
            rebuilt.data_end = footer->data_end;
            rebuilt.chunk_count = footer->chunk_count;
            rebuilt.created_ms = now_ms();
            rebuilt.modified_ms = rebuilt.created_ms;
            header_ = rebuilt;
            header_dirty_ = true;
            footer_dirty_ = false;
            return {};
        }

        header_ = *decoded;

        if (header_.region_x != options.region_x || header_.region_z != options.region_z) {
            return std::unexpected(make_error(TrfErrorCode::CoordinateMismatch,
                "header coordinates do not match the region file name"));
        }
        if (!is_zero(std::span<const std::byte>(options.world_uuid.data(), options.world_uuid.size())) &&
            header_.world_uuid != options.world_uuid) {
            return std::unexpected(make_error(TrfErrorCode::WorldMismatch,
                "region belongs to a different world"));
        }
        if (options.generator_id != 0 && header_.generator_id != options.generator_id) {
            return std::unexpected(make_error(TrfErrorCode::GeneratorMismatch,
                "region was generated by a different generator"));
        }
        if (options.seed != 0 && header_.seed != static_cast<std::int64_t>(options.seed)) {
            return std::unexpected(make_error(TrfErrorCode::WorldMismatch,
                "region seed does not match the world seed"));
        }

        if (footer) {
            if (footer->generation > header_.generation) {
                // Footer written, header write lost: adopt the newer counters (§12 step 4).
                header_.generation = footer->generation;
                header_.data_end = footer->data_end;
                header_.chunk_count = footer->chunk_count;
                header_dirty_ = true;
            }
            if (header_.sealed()) {
                if (footer->sha256 != header_.sha256) {
                    return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                        "header and footer digests disagree"));
                }
                if (is_zero(std::span<const std::byte>(header_.sha256.data(), header_.sha256.size()))) {
                    return std::unexpected(make_error(TrfErrorCode::CorruptHeader,
                        "sealed header carries an all-zero digest"));
                }
            }
            else {
                footer_dirty_ = !is_zero(
                    std::span<const std::byte>(footer->sha256.data(), footer->sha256.size()));
            }
        }
        else {
            if (header_.sealed()) {
                return std::unexpected(make_error(TrfErrorCode::CorruptFooter,
                    "sealed region has no readable footer"));
            }
            footer_dirty_ = true;
        }

        std::vector<std::byte> index_bytes(index_size);
        if (auto read = file_.read_at(header_size, index_bytes); !read) {
            return std::unexpected(read.error());
        }

        for (std::uint32_t id = 0; id < index_capacity; ++id) {
            const std::byte* entry = index_bytes.data() + static_cast<std::size_t>(id) * index_entry_size;
            slots_[id][0] = Slot::decode(std::span<const std::byte, slot_size>(entry, slot_size));
            slots_[id][1] = Slot::decode(
                std::span<const std::byte, slot_size>(entry + slot_size, slot_size));
        }

        return {};
    }

    std::expected<void, TrfError> TrfRegion::rebuild_counters(std::uint64_t file_size) {
        struct Range {
            std::uint64_t begin = 0;
            std::uint64_t end = 0;
            std::uint32_t id = 0;
            int slot = 0;
        };

        for (std::uint32_t id = 0; id < index_capacity; ++id) {
            for (int s = 0; s < 2; ++s) {
                if (!slots_[id][s]) {
                    continue;
                }
                const auto valid = slots_[id][s]->validate(file_size, id);
                if (!valid) {
                    slots_[id][s].reset();
                }
            }
        }

        std::vector<Range> ranges;
        ranges.reserve(index_capacity);
        for (std::uint32_t id = 0; id < index_capacity; ++id) {
            for (int s = 0; s < 2; ++s) {
                const auto& slot = slots_[id][s];
                if (!slot || !slot->addresses_record()) {
                    continue;
                }
                ranges.push_back(Range{
                    slot->record_offset,
                    static_cast<std::uint64_t>(slot->record_offset) + slot->stored_size,
                    id, s });
            }
        }

        std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
            return a.begin < b.begin;
            });

        // The index stores only offsets and sizes, so neither half of an overlap can be
        // identified as the accident: both are dropped (§21).
        for (std::size_t i = 1; i < ranges.size(); ++i) {
            if (ranges[i].begin < ranges[i - 1].end) {
                slots_[ranges[i].id][ranges[i].slot].reset();
                slots_[ranges[i - 1].id][ranges[i - 1].slot].reset();
            }
        }

        std::uint32_t live_chunks = 0;
        std::uint64_t live_bytes = 0;
        std::uint64_t data_end = data_start;

        for (std::uint32_t id = 0; id < index_capacity; ++id) {
            const auto chosen = winner(id);
            if (!chosen) {
                continue;
            }
            if (chosen->addresses_record()) {
                data_end = std::max<std::uint64_t>(data_end,
                    static_cast<std::uint64_t>(chosen->record_offset) + chosen->stored_size);
                if (!chosen->deleted() && !chosen->damaged()) {
                    live_bytes += chosen->stored_size;
                }
            }
            if (!chosen->deleted() && !chosen->damaged()) {
                ++live_chunks;
            }
        }

        // Bytes not covered by a live record are dead space; the clamp keeps a wrapped
        // subtraction from reporting a nearly full region as nearly empty.
        const std::uint64_t span = data_end - data_start;
        const std::uint64_t dead = span > live_bytes ? span - live_bytes : 0;

        if (header_.chunk_count != live_chunks ||
            header_.data_end != static_cast<std::uint32_t>(data_end) ||
            header_.dead_bytes != static_cast<std::uint32_t>(dead)) {
            header_.chunk_count = live_chunks;
            header_.data_end = static_cast<std::uint32_t>(data_end);
            header_.dead_bytes = static_cast<std::uint32_t>(dead);
            header_dirty_ = true;
        }

        return {};
    }

    std::optional<Slot> TrfRegion::winner(std::uint32_t id) const noexcept {
        return winning_slot(slots_[id][0], slots_[id][1]);
    }

    int TrfRegion::shadow_slot(std::uint32_t id) const noexcept {
        if (!slots_[id][0]) {
            return 0;
        }
        if (!slots_[id][1]) {
            return 1;
        }
        const auto chosen = winning_slot(slots_[id][0], slots_[id][1]);
        // Slot B wins only when it is strictly newer; a tie belongs to slot A (§6 rule 2).
        if (chosen && slots_[id][1] && chosen->slot_generation == slots_[id][1]->slot_generation &&
            slots_[id][0] && slots_[id][1]->slot_generation > slots_[id][0]->slot_generation) {
            return 0;
        }
        return 1;
    }

    std::vector<std::byte> TrfRegion::build_index_bytes() const {
        std::vector<std::byte> index_bytes(index_size, std::byte{ 0 });
        for (std::uint32_t id = 0; id < index_capacity; ++id) {
            for (int s = 0; s < 2; ++s) {
                if (!slots_[id][s]) {
                    continue;
                }
                std::array<std::byte, slot_size> encoded{};
                slots_[id][s]->encode(encoded);
                std::memcpy(index_bytes.data() +
                    static_cast<std::size_t>(id) * index_entry_size + static_cast<std::size_t>(s) * slot_size,
                    encoded.data(), slot_size);
            }
        }
        return index_bytes;
    }

    std::expected<void, TrfError> TrfRegion::write_header() {
        std::array<std::byte, header_size> bytes{};
        header_.encode(bytes);
        if (auto written = file_.write_at(0, bytes); !written) {
            return std::unexpected(written.error());
        }
        header_dirty_ = false;
        footer_dirty_ = true;
        return {};
    }

    std::expected<void, TrfError> TrfRegion::write_footer(bool sync) {
        Footer footer = Footer::from_header(header_);
        const auto index_bytes = build_index_bytes();
        footer.index_crc32c = crc32c(index_bytes);

        std::array<std::byte, footer_size> bytes{};
        footer.encode(bytes);
        if (auto written = file_.write_at(file_size_ - footer_size, bytes); !written) {
            return std::unexpected(written.error());
        }
        footer_dirty_ = false;

        if (sync) {
            if (auto barrier = file_.barrier(); !barrier) {
                return std::unexpected(barrier.error());
            }
        }
        return {};
    }

    std::expected<void, TrfError> TrfRegion::write_index() {
        const auto index_bytes = build_index_bytes();
        return file_.write_at(header_size, index_bytes);
    }

    std::expected<void, TrfError> TrfRegion::unseal() {
        header_.flags &= ~header_flags::sealed;
        header_.sha256 = Sha256Digest{};
        if (auto written = write_header(); !written) {
            return std::unexpected(written.error());
        }
        if (auto barrier = file_.barrier(); !barrier) {
            return std::unexpected(barrier.error());
        }
        if (auto written = write_footer(true); !written) {
            return std::unexpected(written.error());
        }
        return {};
    }

    std::expected<LoadResult, TrfError> TrfRegion::load_chunk(std::int32_t cx, std::int32_t cz,
        std::vector<std::byte>& out) {

        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint32_t id = chunk_id(cx, cz);
        const auto chosen = winner(id);

        LoadResult result;
        if (!chosen) {
            return result;
        }

        result.generation = chosen->slot_generation;
        if (chosen->deleted()) {
            result.tombstone = true;
            return result;
        }
        if (chosen->damaged() || !chosen->addresses_record()) {
            return result;
        }

        std::vector<std::byte> stored(chosen->stored_size);
        if (auto read = file_.read_at(chosen->record_offset, stored); !read) {
            return std::unexpected(read.error());
        }

        const std::uint32_t prefix_size = load_u32(stored.data());
        const std::uint32_t prefix_crc = load_u32(stored.data() + 4);
        if (prefix_size != chosen->raw_size) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CrcMismatch, id,
                "record prefix size disagrees with the index"));
        }

        const auto payload = std::span<const std::byte>(stored).subspan(record_prefix_size);
        const std::uint32_t computed = crc32c(payload);
        if (computed != prefix_crc || computed != chosen->stored_crc32c) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CrcMismatch, id,
                "stored payload CRC mismatch"));
        }

        // Only Stored records are produced by this implementation; a compressed record
        // would need a decoder that the format leaves to the caller.
        if (chosen->compression != compression::stored) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CompressionFailed, id,
                "compressed records are not supported by this build"));
        }

        out.assign(payload.begin(), payload.end());
        result.present = true;
        result.compression = chosen->compression;
        result.edited = chosen->edited();
        result.wire_ready = chosen->wire_ready();
        return result;
    }

    std::expected<void, TrfError> TrfRegion::ensure_capacity(std::uint64_t needed_end) {
        if (needed_end + footer_size <= file_size_) {
            return {};
        }

        std::uint64_t target = std::max<std::uint64_t>(file_size_ * 2, needed_end + footer_size);
        target = align_up(target, region_grow_quantum);
        if (target > region_size) {
            target = region_size;
        }
        if (needed_end + footer_size > target) {
            return std::unexpected(make_error(TrfErrorCode::TooLarge,
                "region has reached its 4 GiB capacity"));
        }

        if (auto resized = file_.resize(target); !resized) {
            return std::unexpected(resized.error());
        }
        file_size_ = target;
        footer_dirty_ = true;
        return write_footer(false);
    }

    std::expected<void, TrfError> TrfRegion::append_record(std::uint32_t id,
        std::span<const std::byte> payload, std::uint8_t flags) {

        const std::uint32_t stored_size = static_cast<std::uint32_t>(payload.size()) +
            stored_framing_overhead;
        std::uint64_t offset = align_up(std::max<std::uint64_t>(header_.data_end, data_start),
            data_align);

        if (auto capacity = ensure_capacity(offset + stored_size); !capacity) {
            // Growth is capped by the 4 GiB region, so reclaim dead records and retry once.
            if (auto compacted = compact_locked(); !compacted) {
                return std::unexpected(compacted.error());
            }
            offset = align_up(std::max<std::uint64_t>(header_.data_end, data_start), data_align);
            if (auto retried = ensure_capacity(offset + stored_size); !retried) {
                return std::unexpected(make_chunk_error(TrfErrorCode::TooLarge, id,
                    "region cannot hold this record"));
            }
        }

        std::vector<std::byte> record(stored_size);
        store_u32(record.data(), static_cast<std::uint32_t>(payload.size()));
        store_u32(record.data() + 4, crc32c(payload));
        std::memcpy(record.data() + record_prefix_size, payload.data(), payload.size());

        if (auto written = file_.write_at(offset, record); !written) {
            return std::unexpected(written.error());
        }

        const auto previous = winner(id);

        Slot slot;
        slot.record_offset = static_cast<std::uint32_t>(offset);
        slot.stored_size = stored_size;
        slot.raw_size = static_cast<std::uint32_t>(payload.size());
        slot.compression = compression::stored;
        slot.flags = flags;
        slot.lx = static_cast<std::uint8_t>(id & 31);
        slot.lz = static_cast<std::uint8_t>((id >> 5) & 31);
        slot.stored_crc32c = crc32c(payload);
        slot.raw_crc32c = slot.stored_crc32c;
        slot.slot_generation = next_slot_generation(slots_[id][0], slots_[id][1]);

        const int target = shadow_slot(id);
        std::array<std::byte, slot_size> zero{};
        if (auto written = file_.write_at(slot_offset_of(id, target), zero); !written) {
            return std::unexpected(written.error());
        }

        std::array<std::byte, slot_size> encoded{};
        slot.encode(encoded);
        if (auto written = file_.write_at(slot_offset_of(id, target), encoded); !written) {
            file_.write_at(slot_offset_of(id, target), zero);
            return std::unexpected(written.error());
        }

        slots_[id][target] = slot;

        if (!previous || previous->deleted() || previous->damaged()) {
            header_.chunk_count += 1;
        }
        else {
            header_.dead_bytes += previous->stored_size;
        }
        header_.data_end = static_cast<std::uint32_t>(offset + stored_size);

        // Clamp the dead total so a uint32 wrap can never turn a full region into an
        // apparently empty one (§10.1 step 6).
        const std::uint64_t span = static_cast<std::uint64_t>(header_.data_end) - data_start;
        std::uint64_t live_bytes = 0;
        for (std::uint32_t other = 0; other < index_capacity; ++other) {
            const auto chosen = winner(other);
            if (chosen && chosen->addresses_record() && !chosen->deleted() && !chosen->damaged()) {
                live_bytes += chosen->stored_size;
            }
        }
        const std::uint64_t dead_limit = span > live_bytes ? span - live_bytes : 0;
        if (header_.dead_bytes > dead_limit) {
            header_.dead_bytes = static_cast<std::uint32_t>(dead_limit);
        }

        header_dirty_ = true;
        return file_.flush_stream();
    }

    std::expected<void, TrfError> TrfRegion::save_chunk(std::int32_t cx, std::int32_t cz,
        std::span<const std::byte> payload, std::uint8_t flags) {

        std::lock_guard<std::mutex> lock(mutex_);
        if (read_only_) {
            return std::unexpected(make_error(TrfErrorCode::ReadOnly, "region is open read-only"));
        }
        if (payload.empty() || payload.size() > max_raw_size) {
            return std::unexpected(make_chunk_error(TrfErrorCode::TooLarge, chunk_id(cx, cz),
                "payload size is outside the record bounds"));
        }

        if (header_.sealed()) {
            if (auto cleared = unseal(); !cleared) {
                return std::unexpected(cleared.error());
            }
        }

        return append_record(chunk_id(cx, cz), payload, flags);
    }

    std::expected<bool, TrfError> TrfRegion::delete_chunk(std::int32_t cx, std::int32_t cz) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_only_) {
            return std::unexpected(make_error(TrfErrorCode::ReadOnly, "region is open read-only"));
        }

        if (header_.sealed()) {
            if (auto cleared = unseal(); !cleared) {
                return std::unexpected(cleared.error());
            }
        }

        const std::uint32_t id = chunk_id(cx, cz);
        const auto previous = winner(id);
        // Delete is idempotent: nothing to remove is not an error.
        if (!previous || previous->deleted() || !previous->addresses_record()) {
            return false;
        }

        // The tombstone keeps the dead record's coordinates and sizes so the bytes it
        // orphaned stay accounted for, and it carries no payload flags (§10.4).
        Slot tombstone;
        tombstone.record_offset = previous->record_offset;
        tombstone.stored_size = previous->stored_size;
        tombstone.raw_size = previous->raw_size;
        tombstone.compression = previous->compression;
        tombstone.stored_crc32c = previous->stored_crc32c;
        tombstone.raw_crc32c = previous->raw_crc32c;
        tombstone.lx = previous->lx;
        tombstone.lz = previous->lz;
        tombstone.flags = slot_flags::deleted;
        tombstone.slot_generation = next_slot_generation(slots_[id][0], slots_[id][1]);

        const int target = shadow_slot(id);
        std::array<std::byte, slot_size> zero{};
        if (auto written = file_.write_at(slot_offset_of(id, target), zero); !written) {
            return std::unexpected(written.error());
        }

        std::array<std::byte, slot_size> encoded{};
        tombstone.encode(encoded);
        if (auto written = file_.write_at(slot_offset_of(id, target), encoded); !written) {
            file_.write_at(slot_offset_of(id, target), zero);
            return std::unexpected(written.error());
        }

        slots_[id][target] = tombstone;

        if (header_.chunk_count > 0) {
            header_.chunk_count -= 1;
        }
        header_.dead_bytes += previous->stored_size;

        // Same clamp as the save path: dead_bytes must never wrap around.
        const std::uint64_t span = static_cast<std::uint64_t>(header_.data_end) - data_start;
        std::uint64_t live_bytes = 0;
        for (std::uint32_t other = 0; other < index_capacity; ++other) {
            const auto chosen = winner(other);
            if (chosen && chosen->addresses_record() && !chosen->deleted() && !chosen->damaged()) {
                live_bytes += chosen->stored_size;
            }
        }
        const std::uint64_t dead_limit = span > live_bytes ? span - live_bytes : 0;
        if (header_.dead_bytes > dead_limit) {
            header_.dead_bytes = static_cast<std::uint32_t>(dead_limit);
        }

        header_dirty_ = true;
        if (auto flushed = file_.flush_stream(); !flushed) {
            return std::unexpected(flushed.error());
        }
        return true;
    }

    std::expected<void, TrfError> TrfRegion::compact() {
        std::lock_guard<std::mutex> lock(mutex_);
        return compact_locked();
    }

    std::expected<void, TrfError> TrfRegion::compact_locked() {
        auto temporary = path_;
        temporary += ".tmp";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);

        // Sizing the copy from the surviving records lets compaction shrink a region that
        // accumulated dead space instead of keeping its old footprint.
        std::uint64_t needed = data_start + footer_size;
        for (std::uint32_t probe = 0; probe < index_capacity; ++probe) {
            const auto survivor = winner(probe);
            if (survivor && survivor->addresses_record() && !survivor->deleted() &&
                !survivor->damaged()) {
                needed += survivor->stored_size;
            }
        }
        std::uint64_t target_size = align_up(needed, region_grow_quantum);
        target_size = std::max<std::uint64_t>(target_size, initial_region_size);
        if (target_size > region_size) {
            target_size = region_size;
        }

        if (auto created = TrfFile::create(temporary, target_size); !created) {
            return std::unexpected(created.error());
        }

        auto out = TrfFile::open(temporary);
        if (!out) {
            return std::unexpected(out.error());
        }

        Header next = header_;
        next.generation = header_.generation + 1;
        next.flags &= ~header_flags::sealed;
        next.sha256 = Sha256Digest{};
        next.dead_bytes = 0;
        next.chunk_count = 0;
        next.data_end = static_cast<std::uint32_t>(data_start);
        next.modified_ms = now_ms();

        std::array<std::array<std::optional<Slot>, 2>, index_capacity> rebuilt{};
        std::uint64_t offset = data_start;

        for (std::uint32_t id = 0; id < index_capacity; ++id) {
            const auto chosen = winner(id);
            if (!chosen) {
                continue;
            }

            if (chosen->deleted()) {
                // A delete must survive compaction, and losing the record turns it into
                // the canonical form that needs no bytes (§16).
                Slot tombstone;
                tombstone.lx = chosen->lx;
                tombstone.lz = chosen->lz;
                tombstone.flags = slot_flags::deleted;
                tombstone.slot_generation = chosen->slot_generation + 1;
                rebuilt[id][0] = tombstone;
                continue;
            }
            if (chosen->damaged() || !chosen->addresses_record()) {
                continue;
            }

            std::vector<std::byte> record(chosen->stored_size);
            if (auto read = file_.read_at(chosen->record_offset, record); !read) {
                return std::unexpected(read.error());
            }
            if (auto written = out->write_at(offset, record); !written) {
                return std::unexpected(written.error());
            }

            Slot slot = *chosen;
            slot.record_offset = static_cast<std::uint32_t>(offset);
            slot.slot_generation = chosen->slot_generation + 1;
            rebuilt[id][0] = slot;

            offset = align_up(offset + chosen->stored_size, data_align);
            next.chunk_count += 1;
        }

        next.data_end = static_cast<std::uint32_t>(offset);

        std::vector<std::byte> index_bytes(index_size, std::byte{ 0 });
        for (std::uint32_t id = 0; id < index_capacity; ++id) {
            for (int s = 0; s < 2; ++s) {
                if (!rebuilt[id][s]) {
                    continue;
                }
                std::array<std::byte, slot_size> encoded{};
                rebuilt[id][s]->encode(encoded);
                std::memcpy(index_bytes.data() +
                    static_cast<std::size_t>(id) * index_entry_size + static_cast<std::size_t>(s) * slot_size,
                    encoded.data(), slot_size);
            }
        }

        if (auto written = out->write_at(header_size, index_bytes); !written) {
            return std::unexpected(written.error());
        }

        Footer footer = Footer::from_header(next);
        footer.index_crc32c = crc32c(index_bytes);
        footer.sha256 = Sha256Digest{};
        std::array<std::byte, footer_size> footer_bytes{};
        footer.encode(footer_bytes);
        if (auto written = out->write_at(target_size - footer_size, footer_bytes); !written) {
            return std::unexpected(written.error());
        }

        std::array<std::byte, header_size> header_bytes{};
        next.encode(header_bytes);
        if (auto written = out->write_at(0, header_bytes); !written) {
            return std::unexpected(written.error());
        }

        if (auto barrier = out->barrier(); !barrier) {
            return std::unexpected(barrier.error());
        }
        out->close();

#ifdef _WIN32
        // ReplaceFileW refuses a target that still has open handles, so the region
        // releases its own descriptor before the swap (§16 step 4).
        file_.close();
#endif

        if (auto replaced = TrfFile::replace(temporary, path_); !replaced) {
            return std::unexpected(replaced.error());
        }
        TrfFile::sync_directory(path_.parent_path());

        file_.close();
        auto reopened = TrfFile::open(path_);
        if (!reopened) {
            return std::unexpected(reopened.error());
        }

        file_ = std::move(*reopened);
        header_ = next;
        slots_ = rebuilt;
        file_size_ = target_size;
        header_dirty_ = false;
        footer_dirty_ = false;
        return {};
    }

    std::expected<void, TrfError> TrfRegion::seal() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_only_) {
            return std::unexpected(make_error(TrfErrorCode::ReadOnly, "region is open read-only"));
        }
        if (header_.sealed()) {
            return {};
        }

        header_.flags |= header_flags::sealed;
        header_.generation += 1;
        header_.sha256 = Sha256Digest{};
        header_.modified_ms = now_ms();

        std::array<std::byte, header_size> header_bytes{};
        header_.encode(header_bytes);

        const auto index_bytes = build_index_bytes();

        std::vector<std::byte> covered;
        covered.reserve(header_size + index_size);
        covered.insert(covered.end(), header_bytes.begin(), header_bytes.begin() + header_crc_coverage + 4);
        covered.insert(covered.end(), header_bytes.begin() + header_reserved_offset, header_bytes.end());
        covered.insert(covered.end(), index_bytes.begin(), index_bytes.end());

        const Sha256Digest digest = sha256(covered);

        Footer footer = Footer::from_header(header_);
        footer.sha256 = digest;
        footer.index_crc32c = crc32c(index_bytes);
        std::array<std::byte, footer_size> footer_bytes{};
        footer.encode(footer_bytes);

        if (auto written = file_.write_at(file_size_ - footer_size, footer_bytes); !written) {
            return std::unexpected(written.error());
        }
        if (auto barrier = file_.barrier(); !barrier) {
            header_.flags &= ~header_flags::sealed;
            header_.generation -= 1;
            return std::unexpected(barrier.error());
        }

        header_.sha256 = digest;
        if (auto written = write_header(); !written) {
            return std::unexpected(written.error());
        }
        footer_dirty_ = false;
        return file_.barrier();
    }

    std::expected<void, TrfError> TrfRegion::flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.valid()) {
            return {};
        }
        if (footer_dirty_) {
            if (auto written = write_footer(false); !written) {
                return std::unexpected(written.error());
            }
        }
        if (header_dirty_) {
            if (auto written = write_header(); !written) {
                return std::unexpected(written.error());
            }
        }
        return file_.barrier();
    }

}
