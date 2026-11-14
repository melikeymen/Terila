#include "nbt/nbt_reader.hpp"
#include "tests/fuzz/fuzz_support.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

TERILA_FUZZ_INITIALIZE()

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr) return 0;
    const auto bytes = std::span<const std::uint8_t>(data, size);
    for (auto format : {terila::nbt::StringFormat::Network, terila::nbt::StringFormat::Classic}) {
        terila::nbt::Reader reader(bytes, format);
        (void)reader.read_root_compound();
    }
    return 0;
}
