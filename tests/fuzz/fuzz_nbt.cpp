#include "nbt/nbt_codec.hpp"
#include "tests/fuzz/fuzz_support.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

TERILA_FUZZ_INITIALIZE()

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr) return 0;
    const auto bytes = std::span<const std::uint8_t>(data, size);
    for (auto layout : {terila::nbt::Layout::Unnamed, terila::nbt::Layout::Named}) {
        terila::nbt::NbtReader reader(bytes, layout);
        (void)reader.root();
    }
    return 0;
}
