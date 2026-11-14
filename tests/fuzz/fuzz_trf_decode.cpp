// libFuzzer target: clang -fsanitize=fuzzer,address, built with
// -DTERILA_ENABLE_FUZZING=ON. Run e.g. ./fuzz_trf_decode -max_total_time=60
#include "tests/fuzz/trf_probe.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    terila::test::probe_trf_decoders(data, size);
    return 0;
}
