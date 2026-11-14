// libFuzzer target for every serverbound packet reader.
#include "tests/fuzz/fuzz_support.hpp"
#include "tests/fuzz/protocol_probe.hpp"

#include <cstddef>
#include <cstdint>

TERILA_FUZZ_INITIALIZE()

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    terila::test::probe_protocol(data, size);
    return 0;
}
