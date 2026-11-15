#include "tag/codec.hpp"
#include "tests/fuzz/fuzz_support.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

TERILA_FUZZ_INITIALIZE()

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr) return 0;
    const auto bytes = std::span<const std::uint8_t>(data, size);
    for (auto layout : {terila::tag::Layout::Unnamed, terila::tag::Layout::Named}) {
        terila::tag::TagReader reader(bytes, layout);
        (void)reader.root();
    }
    return 0;
}
