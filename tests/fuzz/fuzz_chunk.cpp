// libFuzzer target for the chunk payload decoder and the encoder it feeds.
#include "tests/fuzz/chunk_probe.hpp"
#include "tests/fuzz/fuzz_support.hpp"

#include <cstddef>
#include <cstdint>

TERILA_FUZZ_INITIALIZE()

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    terila::test::probe_chunk(data, size);
    return 0;
}
