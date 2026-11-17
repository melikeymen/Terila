#include <gtest/gtest.h>

#include "world/trf/trf_crc32c.hpp"
#include "world/trf/trf_format.hpp"
#include "world/trf/trf_header.hpp"
#include "world/trf/trf_region.hpp"
#include "world/trf/trf_slot.hpp"
#include "world/trf/trf_store.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

    namespace fs = std::filesystem;
    namespace trf = terila::world::trf;

    class TempDir {
    public:
        TempDir() {
            static std::atomic<std::uint64_t> counter{ 0 };
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = fs::temp_directory_path() / ("terila_trf_" + std::to_string(stamp) + "_" +
                std::to_string(counter++));
            fs::create_directories(path_);
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }

        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const fs::path& path() const { return path_; }

    private:
        fs::path path_;
    };

    trf::RegionOptions region_options(std::int32_t rx = 0, std::int32_t rz = 0) {
        trf::RegionOptions options;
        options.region_x = rx;
        options.region_z = rz;
        options.seed = 1337;
        return options;
    }

    std::vector<std::byte> payload_bytes(std::size_t size, std::uint8_t salt) {
        std::vector<std::byte> bytes(size);
        for (std::size_t i = 0; i < size; ++i) {
            bytes[i] = static_cast<std::byte>((i * 31u + salt) & 0xFFu);
        }
        return bytes;
    }

    std::span<const std::byte> as_span(const std::vector<std::byte>& bytes) {
        return std::span<const std::byte>(bytes);
    }

    std::span<const std::byte> as_span(std::string_view text) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(text.data()), text.size());
    }

} // namespace

// --- §9 checksums -----------------------------------------------------------

TEST(TrfCrc32cTest, MatchesTheStandardCheckValue) {
    EXPECT_EQ(trf::crc32c(as_span(std::string_view("123456789"))), 0xE3069283u);
    EXPECT_EQ(trf::crc32c_software(as_span(std::string_view("123456789"))), 0xE3069283u);
}

TEST(TrfCrc32cTest, EmptyInputIsZero) {
    EXPECT_EQ(trf::crc32c({}), 0u);
    EXPECT_EQ(trf::crc32c_software({}), 0u);
}

TEST(TrfCrc32cTest, HardwareAndSoftwareAgree) {
    const auto bytes = payload_bytes(4096, 0x5A);
    EXPECT_EQ(trf::crc32c(as_span(bytes)), trf::crc32c_software(as_span(bytes)));
}

TEST(TrfCrc32cTest, ChainedSeedMatchesTheWholeBuffer) {
    const auto head = trf::crc32c(as_span(std::string_view("123")));
    EXPECT_EQ(trf::crc32c(as_span(std::string_view("456789")), head),
        trf::crc32c(as_span(std::string_view("123456789"))));
}

// --- §5 header -------------------------------------------------------------

TEST(TrfHeaderTest, RoundTripsEveryField) {
    trf::Header header;
    header.flags = trf::header_flags::generated | trf::header_flags::sealed;
    header.region_x = -3;
    header.region_z = 7;
    header.chunk_count = 42;
    header.data_end = static_cast<std::uint32_t>(trf::data_start + 8192);
    header.dead_bytes = 512;
    header.generation = 11;
    header.created_ms = 1234;
    header.modified_ms = 5678;
    header.seed = -99;
    header.world_uuid[3] = std::byte{ 0xAB };
    header.generator_version = 2;
    header.generator_id = 0xDEADBEEFu;
    header.default_compression = trf::compression::stored;

    std::array<std::byte, trf::header_size> bytes{};
    header.encode(bytes);

    const auto decoded = trf::Header::decode(bytes);
    ASSERT_TRUE(decoded.has_value()) << trf::to_string(decoded.error().code);
    EXPECT_EQ(decoded->version_major, header.version_major);
    EXPECT_EQ(decoded->flags, header.flags);
    EXPECT_EQ(decoded->region_x, header.region_x);
    EXPECT_EQ(decoded->region_z, header.region_z);
    EXPECT_EQ(decoded->chunk_count, header.chunk_count);
    EXPECT_EQ(decoded->data_end, header.data_end);
    EXPECT_EQ(decoded->dead_bytes, header.dead_bytes);
    EXPECT_EQ(decoded->generation, header.generation);
    EXPECT_EQ(decoded->created_ms, header.created_ms);
    EXPECT_EQ(decoded->modified_ms, header.modified_ms);
    EXPECT_EQ(decoded->seed, header.seed);
    EXPECT_EQ(decoded->world_uuid, header.world_uuid);
    EXPECT_EQ(decoded->generator_version, header.generator_version);
    EXPECT_EQ(decoded->generator_id, header.generator_id);
    EXPECT_EQ(decoded->default_compression, header.default_compression);
}

TEST(TrfHeaderTest, RejectsATamperedByte) {
    trf::Header header;
    std::array<std::byte, trf::header_size> bytes{};
    header.encode(bytes);
    bytes[0x10] ^= std::byte{ 0xFF };

    const auto decoded = trf::Header::decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, trf::TrfErrorCode::CorruptHeader);
}

TEST(TrfHeaderTest, RejectsAnUnsupportedMajorVersion) {
    trf::Header header;
    header.version_major = 2;
    std::array<std::byte, trf::header_size> bytes{};
    header.encode(bytes);

    const auto decoded = trf::Header::decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, trf::TrfErrorCode::UnsupportedFormat);
}

// --- §6/§7.3 footer --------------------------------------------------------

TEST(TrfFooterTest, RoundTripsAndMatchesTheHeader) {
    trf::Header header;
    header.chunk_count = 9;
    header.data_end = static_cast<std::uint32_t>(trf::data_start + 4096);
    header.generation = 3;
    header.region_x = -2;
    header.region_z = 5;

    const auto footer = trf::Footer::from_header(header);
    EXPECT_EQ(footer.chunk_count, header.chunk_count);
    EXPECT_EQ(footer.data_end, header.data_end);
    EXPECT_EQ(footer.generation, header.generation);

    std::array<std::byte, trf::footer_size> bytes{};
    footer.encode(bytes);
    const auto decoded = trf::Footer::decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->chunk_count, footer.chunk_count);
    EXPECT_EQ(decoded->region_x, footer.region_x);
    EXPECT_EQ(decoded->region_z, footer.region_z);
}

TEST(TrfFooterTest, RejectsATamperedByte) {
    trf::Footer footer;
    std::array<std::byte, trf::footer_size> bytes{};
    footer.encode(bytes);
    bytes[0x08] ^= std::byte{ 0x7F };
    EXPECT_FALSE(trf::Footer::decode(bytes).has_value());
}

// --- §6 index slots -------------------------------------------------------

TEST(TrfSlotTest, RoundTripsTheAddressingFields) {
    trf::Slot slot;
    slot.record_offset = static_cast<std::uint32_t>(trf::data_start + 64);
    slot.stored_size = 512;
    slot.raw_size = 504;
    slot.compression = trf::compression::stored;
    slot.flags = static_cast<std::uint8_t>(
        trf::slot_flags::edited | trf::slot_flags::wire_ready);
    slot.lx = 3;
    slot.lz = 4;
    slot.slot_generation = 9;

    std::array<std::byte, trf::slot_size> bytes{};
    slot.encode(bytes);

    const auto decoded = trf::Slot::decode(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->record_offset, slot.record_offset);
    EXPECT_EQ(decoded->stored_size, slot.stored_size);
    EXPECT_EQ(decoded->raw_size, slot.raw_size);
    EXPECT_EQ(decoded->compression, slot.compression);
    EXPECT_EQ(decoded->flags, slot.flags);
    EXPECT_EQ(decoded->lx, slot.lx);
    EXPECT_EQ(decoded->lz, slot.lz);
    EXPECT_EQ(decoded->slot_generation, slot.slot_generation);
    EXPECT_TRUE(decoded->addresses_record());
    EXPECT_TRUE(decoded->edited());
    EXPECT_TRUE(decoded->wire_ready());
    EXPECT_FALSE(decoded->deleted());
}

TEST(TrfSlotTest, RejectsATamperedSlot) {
    trf::Slot slot;
    slot.record_offset = static_cast<std::uint32_t>(trf::data_start);
    slot.stored_size = 64;
    slot.raw_size = 56;
    slot.compression = trf::compression::stored;

    std::array<std::byte, trf::slot_size> bytes{};
    slot.encode(bytes);
    bytes[0] ^= std::byte{ 0x01 };

    EXPECT_FALSE(trf::Slot::decode(bytes).has_value());
}

TEST(TrfSlotTest, RecognisesTheCanonicalTombstone) {
    trf::Slot tombstone;
    tombstone.flags = trf::slot_flags::deleted;
    EXPECT_TRUE(tombstone.is_canonical_tombstone());
    EXPECT_TRUE(tombstone.deleted());
    EXPECT_FALSE(tombstone.addresses_record());

    trf::Slot pointing = tombstone;
    pointing.record_offset = 4096;
    EXPECT_FALSE(pointing.is_canonical_tombstone());
}

TEST(TrfSlotTest, RejectsARecordOutsideTheFile) {
    trf::Slot slot;
    slot.record_offset = 0xFFFFFF00u;
    slot.stored_size = 1024;
    slot.raw_size = 1016;
    slot.compression = trf::compression::stored;

    const auto valid = slot.validate(1024 * 1024, trf::no_chunk_id);
    EXPECT_FALSE(valid.has_value());
}

// --- §10/§13 region storage -----------------------------------------------

TEST(TrfRegionTest, SavesAndLoadsPayloadBytes) {
    TempDir dir;
    auto opened = trf::TrfRegion::open(dir.path() / "r.0.0.trf", region_options());
    ASSERT_TRUE(opened.has_value()) << trf::to_string(opened.error().code);
    auto& region = **opened;

    const auto payload = payload_bytes(2048, 7);
    ASSERT_TRUE(region.save_chunk(3, 5, as_span(payload),
        trf::chunk_flags(true, true, false)).has_value());

    std::vector<std::byte> loaded;
    const auto result = region.load_chunk(3, 5, loaded);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->present);
    EXPECT_FALSE(result->tombstone);
    EXPECT_TRUE(result->edited);
    EXPECT_TRUE(result->wire_ready);
    EXPECT_EQ(loaded, payload);
}

TEST(TrfRegionTest, ReportsAbsentChunksWithoutAnError) {
    TempDir dir;
    auto opened = trf::TrfRegion::open(dir.path() / "r.0.0.trf", region_options());
    ASSERT_TRUE(opened.has_value());
    auto& region = **opened;

    std::vector<std::byte> loaded;
    const auto result = region.load_chunk(1, 2, loaded);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->present);
    EXPECT_FALSE(result->tombstone);
    EXPECT_TRUE(loaded.empty());
}

TEST(TrfRegionTest, DeleteWritesATombstoneAndIsIdempotent) {
    TempDir dir;
    auto opened = trf::TrfRegion::open(dir.path() / "r.0.0.trf", region_options());
    ASSERT_TRUE(opened.has_value());
    auto& region = **opened;

    ASSERT_TRUE(region.save_chunk(4, 4, as_span(payload_bytes(256, 1)),
        trf::chunk_flags(false, true, false)).has_value());

    const auto deleted = region.delete_chunk(4, 4);
    ASSERT_TRUE(deleted.has_value());
    EXPECT_TRUE(*deleted);

    std::vector<std::byte> loaded;
    const auto result = region.load_chunk(4, 4, loaded);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->present);
    EXPECT_TRUE(result->tombstone);

    const auto again = region.delete_chunk(4, 4);
    ASSERT_TRUE(again.has_value());
    EXPECT_FALSE(*again);
}

TEST(TrfRegionTest, SurvivesAReopen) {
    TempDir dir;
    const auto path = dir.path() / "r.0.0.trf";
    const auto payload = payload_bytes(1024, 3);

    {
        auto opened = trf::TrfRegion::open(path, region_options());
        ASSERT_TRUE(opened.has_value());
        auto& region = **opened;
        ASSERT_TRUE(region.save_chunk(0, 0, as_span(payload),
            trf::chunk_flags(true, true, false)).has_value());
        region.close();
    }

    auto reopened = trf::TrfRegion::open(path, region_options());
    ASSERT_TRUE(reopened.has_value()) << trf::to_string(reopened.error().code);
    auto& region = **reopened;
    EXPECT_EQ(region.header().chunk_count, 1u);

    std::vector<std::byte> loaded;
    const auto result = region.load_chunk(0, 0, loaded);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->present);
    EXPECT_EQ(loaded, payload);
}

TEST(TrfRegionTest, SealMarksTheFileAndSavingUnsealsIt) {
    TempDir dir;
    const auto path = dir.path() / "r.0.0.trf";

    {
        auto opened = trf::TrfRegion::open(path, region_options());
        ASSERT_TRUE(opened.has_value());
        auto& region = **opened;
        ASSERT_TRUE(region.save_chunk(0, 0, as_span(payload_bytes(128, 1)),
            trf::chunk_flags(false, true, false)).has_value());
        ASSERT_TRUE(region.seal().has_value());
        EXPECT_TRUE(region.sealed());
        region.close();
    }

    auto reopened = trf::TrfRegion::open(path, region_options());
    ASSERT_TRUE(reopened.has_value());
    auto& region = **reopened;
    EXPECT_TRUE(region.sealed());

    ASSERT_TRUE(region.save_chunk(1, 0, as_span(payload_bytes(128, 2)),
        trf::chunk_flags(false, true, false)).has_value());
    EXPECT_FALSE(region.sealed());
}

TEST(TrfRegionTest, SecondOpenOfTheSameFileIsLockedOut) {
    TempDir dir;
    const auto path = dir.path() / "r.0.0.trf";

    auto first = trf::TrfRegion::open(path, region_options());
    ASSERT_TRUE(first.has_value());

    const auto second = trf::TrfRegion::open(path, region_options());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, trf::TrfErrorCode::Locked);
}

TEST(TrfRegionTest, RejectsAWrongRegionCoordinate) {
    TempDir dir;
    const auto path = dir.path() / "r.0.0.trf";

    {
        auto first = trf::TrfRegion::open(path, region_options(0, 0));
        ASSERT_TRUE(first.has_value());
        (*first)->close();
    }

    const auto wrong = trf::TrfRegion::open(path, region_options(1, 0));
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error().code, trf::TrfErrorCode::CoordinateMismatch);
}

TEST(TrfRegionTest, CompactReclaimsDeadBytesAndKeepsTheLatestPayload) {
    TempDir dir;
    auto opened = trf::TrfRegion::open(dir.path() / "r.0.0.trf", region_options());
    ASSERT_TRUE(opened.has_value());
    auto& region = **opened;

    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(region.save_chunk(0, 0,
            as_span(payload_bytes(1024, static_cast<std::uint8_t>(i))),
            trf::chunk_flags(false, true, false)).has_value());
    }

    const auto dead_before = region.header().dead_bytes;
    ASSERT_GT(dead_before, 0u);
    ASSERT_TRUE(region.compact().has_value());
    EXPECT_LT(region.header().dead_bytes, dead_before);

    std::vector<std::byte> loaded;
    const auto result = region.load_chunk(0, 0, loaded);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->present);
    EXPECT_EQ(loaded, payload_bytes(1024, 49));
}

TEST(TrfRegionTest, HoldsEveryChunkInItsIndex) {
    TempDir dir;
    auto opened = trf::TrfRegion::open(dir.path() / "r.0.0.trf", region_options());
    ASSERT_TRUE(opened.has_value());
    auto& region = **opened;

    for (std::int32_t cx = 0; cx < 32; ++cx) {
        for (std::int32_t cz = 0; cz < 32; ++cz) {
            ASSERT_TRUE(region.save_chunk(cx, cz, as_span(payload_bytes(16, 0)),
                trf::chunk_flags(false, true, false)).has_value());
        }
    }
    EXPECT_EQ(region.header().chunk_count, 1024u);

    for (std::int32_t cz = 0; cz < 32; ++cz) {
        for (std::int32_t cx = 0; cx < 32; ++cx) {
            std::vector<std::byte> loaded;
            const auto result = region.load_chunk(cx, cz, loaded);
            ASSERT_TRUE(result.has_value());
            EXPECT_TRUE(result->present) << cx << "," << cz;
        }
    }
}

// --- §18 ChunkStore -------------------------------------------------------

TEST(TrfChunkStoreTest, PersistsChunksAcrossRegionFiles) {
    TempDir dir;
    trf::WorldIdentity identity;
    identity.seed = 7;

    trf::ChunkStore store(dir.path(), identity);
    const auto near = payload_bytes(256, 1);
    const auto far = payload_bytes(256, 2);

    ASSERT_TRUE(store.save_chunk(1, 1, as_span(near),
        trf::chunk_flags(false, true, false)).has_value());
    ASSERT_TRUE(store.save_chunk(40, 1, as_span(far),
        trf::chunk_flags(false, true, false)).has_value());

    std::vector<std::byte> loaded;
    auto result = store.load_chunk(1, 1, loaded);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->present);
    EXPECT_EQ(loaded, near);

    result = store.load_chunk(40, 1, loaded);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->present);
    EXPECT_EQ(loaded, far);
}

TEST(TrfChunkStoreTest, KeepsTheWorldDescriptorStable) {
    TempDir dir;

    const auto first = trf::load_or_create_world(dir.path());
    ASSERT_TRUE(first.has_value()) << trf::to_string(first.error().code);
    const auto second = trf::load_or_create_world(dir.path());
    ASSERT_TRUE(second.has_value());

    EXPECT_EQ(first->world_uuid, second->world_uuid);
    EXPECT_EQ(first->generator_id, second->generator_id);
    EXPECT_EQ(first->generator_version, second->generator_version);
}

TEST(TrfChunkStoreTest, KeepsTheSeedTheWorldWasCreatedWith) {
    TempDir dir;

    const auto first = trf::load_or_create_world(dir.path(), 424242u);
    ASSERT_TRUE(first.has_value()) << trf::to_string(first.error().code);
    EXPECT_EQ(first->seed, 424242u);

    // A later run that names another seed keeps the world's own terrain.
    const auto second = trf::load_or_create_world(dir.path(), 999u);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->seed, 424242u);
}

TEST(TrfChunkStoreTest, DrawsARandomSeedForANewWorld) {
    TempDir first_dir;
    TempDir second_dir;

    const auto first = trf::load_or_create_world(first_dir.path(), std::nullopt);
    const auto second = trf::load_or_create_world(second_dir.path(), std::nullopt);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    EXPECT_NE(first->seed, 0u);
    EXPECT_NE(first->seed, second->seed);
}

TEST(TrfChunkStoreTest, KeepsSeedZeroAsARealSeed) {
    TempDir dir;

    // level-seed=0 names a world, it does not ask for a random one.
    const auto created = trf::load_or_create_world(dir.path(), 0u);
    ASSERT_TRUE(created.has_value()) << trf::to_string(created.error().code);
    EXPECT_EQ(created->seed, 0u);

    const auto reopened = trf::load_or_create_world(dir.path(), std::nullopt);
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ(reopened->seed, 0u) << "the descriptor seed was overwritten";
}

TEST(TrfFormatTest, RegionCoordinateFloorsTowardsMinusInfinity) {
    EXPECT_EQ(trf::region_coordinate(0), 0);
    EXPECT_EQ(trf::region_coordinate(31), 0);
    EXPECT_EQ(trf::region_coordinate(32), 1);
    EXPECT_EQ(trf::region_coordinate(-1), -1);
    EXPECT_EQ(trf::region_coordinate(-32), -1);
    EXPECT_EQ(trf::region_coordinate(-33), -2);
}

TEST(TrfFormatTest, ChunkIdsStayInsideTheIndex) {
    EXPECT_EQ(trf::chunk_id(0, 0), 0u);
    EXPECT_EQ(trf::chunk_id(31, 31), 1023u);
    EXPECT_EQ(trf::chunk_id(-1, -1), trf::chunk_id(31, 31));
    for (std::int32_t cx = -64; cx < 64; cx += 7) {
        for (std::int32_t cz = -64; cz < 64; cz += 5) {
            EXPECT_LT(trf::chunk_id(cx, cz), trf::index_capacity);
        }
    }
}

#ifdef _WIN32
#include <windows.h>

// A region is a fixed-size file that is mostly empty, so it has to be sparse: the old
// _chsize_s extension wrote zeros through the whole file and allocated every block of it.
TEST(TrfFileTest, RegionFilesStaySparseWhenTheyGrow) {
    TempDir dir;
    const auto path = dir.path() / "sparse.trf";

    DWORD volume_flags = 0;
    const auto root = path.root_path().wstring();
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, &volume_flags,
            nullptr, 0)) {
        GTEST_SKIP() << "cannot query the volume holding " << path.string();
    }
    if ((volume_flags & FILE_SUPPORTS_SPARSE_FILES) == 0) {
        GTEST_SKIP() << "volume does not support sparse files";
    }

    ASSERT_TRUE(trf::TrfFile::create(path, 64ull * 1024 * 1024).has_value());

    std::uint32_t high = 0;
    const auto low = GetCompressedFileSizeW(path.wstring().c_str(),
        reinterpret_cast<LPDWORD>(&high));
    ASSERT_NE(low, INVALID_FILE_SIZE);
    const auto allocated = (static_cast<std::uint64_t>(high) << 32) | low;

    EXPECT_EQ(std::filesystem::file_size(path), 64ull * 1024 * 1024);
    EXPECT_EQ(allocated, 0u) << "the empty region allocated disk space";
}
#endif
