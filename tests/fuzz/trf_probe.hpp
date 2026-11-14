#pragma once

#include "world/chunk_decoder.hpp"
#include "world/trf/trf_format.hpp"
#include "world/trf/trf_header.hpp"
#include "world/trf/trf_slot.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace terila::test {

    namespace trf = terila::world::trf;

    // Feeds arbitrary bytes to the decoders. They may reject anything, but they must
    // never read outside the buffer or crash.
    inline void probe_trf_decoders(const std::uint8_t* data, std::size_t size) {
        const auto* bytes = reinterpret_cast<const std::byte*>(data);

        if (size >= trf::header_size) {
            (void)trf::Header::decode(
                std::span<const std::byte, trf::header_size>(bytes, trf::header_size));
        }
        if (size >= trf::footer_size) {
            (void)trf::Footer::decode(
                std::span<const std::byte, trf::footer_size>(bytes, trf::footer_size));
        }
        if (size >= trf::slot_size) {
            (void)trf::Slot::decode(
                std::span<const std::byte, trf::slot_size>(bytes, trf::slot_size));
        }

        (void)terila::world::decode::decode(data, size, 24);
    }

} // namespace terila::test
