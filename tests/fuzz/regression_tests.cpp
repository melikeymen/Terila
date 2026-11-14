#include <gtest/gtest.h>

#include "tests/fuzz/chunk_probe.hpp"
#include "tests/fuzz/protocol_probe.hpp"
#include "tests/fuzz/trf_probe.hpp"
#include "world/chunk_decoder.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

    namespace fs = std::filesystem;

    fs::path regression_dir() {
        return fs::path(TERILA_TEST_DATA_DIR) / "fuzz" / "regressions";
    }

    std::vector<std::uint8_t> read_bytes(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>(
            std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

} // namespace

// Every input a fuzzer ever found a crash with is kept in the corpus and replayed by the
// normal test run, so the same bytes can never reach a release unnoticed again.
TEST(FuzzRegressionTest, ReplaysEverySavedInput) {
    const auto dir = regression_dir();
    ASSERT_TRUE(fs::is_directory(dir)) << "missing regression corpus: " << dir.string();

    int replayed = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".bin") {
            continue;
        }
        const auto bytes = read_bytes(entry.path());
        ASSERT_FALSE(bytes.empty()) << entry.path().string();

        terila::test::probe_trf_decoders(bytes.data(), bytes.size());
        terila::test::probe_protocol(bytes.data(), bytes.size());
        terila::test::probe_chunk(bytes.data(), bytes.size());
        ++replayed;
    }
    EXPECT_GT(replayed, 0) << "the regression corpus is empty";
}

// First finding:/a palette bit width above 64 made the long packing divide by zero
// (64 / bits == 0), so any chunk stream could stop the process with SIGFPE.
TEST(FuzzRegressionTest, ChunkDecoderRejectsAHostilePaletteWidth) {
    const std::vector<std::uint8_t> input{
        0, 0, 0, 0, 0, 0, 0, 0,    // chunk x, z
        0,                         // heightmap count
        5,                         // chunk data length
        0, 0, 0, 0,                // block and fluid counts
        200                        // palette bit width
    };

    EXPECT_FALSE(terila::world::decode::decode(input.data(), input.size(), 24).has_value());
    terila::test::probe_chunk(input.data(), input.size());
}

// Second finding: the heightmap count reached reserve() unvalidated, so a 12 byte packet
// asked for gigabytes and threw bad_alloc out of the decode.
TEST(FuzzRegressionTest, ChunkDecoderRejectsAHugeHeightmapCount) {
    const std::vector<std::uint8_t> input{
        0, 0, 0, 0, 0, 0, 0, 0,          // chunk x, z
        0xFF, 0xFF, 0xFF, 0x7F           // heightmap count: 268435455
    };

    EXPECT_FALSE(terila::world::decode::decode(input.data(), input.size(), 24).has_value());
    terila::test::probe_chunk(input.data(), input.size());
}
