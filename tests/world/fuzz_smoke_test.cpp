#include <gtest/gtest.h>

#include "tests/fuzz/chunk_probe.hpp"
#include "tests/fuzz/protocol_probe.hpp"
#include "tests/fuzz/trf_probe.hpp"
#include "world/trf/trf_header.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {

    namespace trf = terila::world::trf;

} // namespace

// The libFuzzer target needs Clang; this keeps the same coverage in every build and
// fails loudly (instead of silently corrupting memory) when a decoder regresses.
TEST(TrfFuzzSmokeTest, RandomBytesNeverCrashTheDecoders) {
    std::mt19937 rng(0xC0FFEEu);
    std::vector<std::uint8_t> buffer(trf::header_size);

    for (int iteration = 0; iteration < 400; ++iteration) {
        const std::size_t length = 1 + (rng() % 2048);
        for (std::size_t i = 0; i < length; ++i) {
            buffer[i] = static_cast<std::uint8_t>(rng() & 0xFFu);
        }
        terila::test::probe_trf_decoders(buffer.data(), length);
        terila::test::probe_protocol(buffer.data(), length);
        terila::test::probe_chunk(buffer.data(), length);
    }
}

TEST(TrfFuzzSmokeTest, MutationsOfAValidHeaderAreRejectedOrAcceptedSafely) {
    trf::Header header;
    header.region_x = 2;
    header.region_z = -1;
    header.chunk_count = 3;

    std::array<std::byte, trf::header_size> bytes{};
    header.encode(bytes);

    std::mt19937 rng(0x1234u);
    for (int iteration = 0; iteration < 500; ++iteration) {
        auto mutated = bytes;
        const std::size_t flips = 1 + (rng() % 4);
        for (std::size_t i = 0; i < flips; ++i) {
            const std::size_t offset = rng() % mutated.size();
            mutated[offset] ^= static_cast<std::byte>(1u << (rng() % 8));
        }

        const auto decoded = trf::Header::decode(mutated);
        if (decoded.has_value()) {
            // A header that survives its CRC must still describe a sane layout.
            EXPECT_GE(decoded->data_end, trf::data_start);
            EXPECT_LE(decoded->index_capacity, trf::index_capacity);
        }
    }
}
