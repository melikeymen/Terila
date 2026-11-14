#pragma once

#include "world/chunk_decoder.hpp"
#include "world/chunk_encoder.hpp"

#include <cstddef>
#include <cstdint>

namespace terila::test {

    // Decodes the blob the way a chunk packet delivers it, then re-encodes whatever came
    // out so the encoder also sees the hostile palettes the decoder accepted.
    inline void probe_chunk(const std::uint8_t* data, std::size_t size) {
        for (const std::int32_t sections : { 1, 24 }) {
            if (auto chunk = terila::world::decode::decode(data, size, sections)) {
                const auto encoded = terila::world::encode::encode(*chunk);
                (void)terila::world::decode::decode(encoded.data(), encoded.size(), sections);
            }
        }
    }

} // namespace terila::test
