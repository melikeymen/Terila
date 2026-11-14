// libFuzzer target: built by the x64-fuzz presets. Arguments go to libFuzzer, e.g.
//   fuzz_trf_decode.exe corpus -jobs=4 -workers=4
#include "tests/fuzz/fuzz_support.hpp"
#include "tests/fuzz/trf_probe.hpp"

#include <cstddef>
#include <cstdint>

TERILA_FUZZ_INITIALIZE()

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    terila::test::probe_trf_decoders(data, size);
    return 0;
}
