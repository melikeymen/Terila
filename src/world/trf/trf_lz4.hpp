#pragma once

// LZ4 block codec for TRF records (codec id 2, §15). The format leaves the codec to the
// reader, so the block is the standard LZ4 sequence layout: a token nibble pair, literal
// bytes, a little-endian match offset and the match length. Records keep their raw size in
// both the slot and the record prefix, so decoding is exactly bounded: a sequence may never
// write past that size, no matter what the compressed bytes say.

#include "world/trf/trf_error.hpp"
#include "world/trf/trf_format.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace terila::world::trf {

    // Below this a record is smaller than the framing it would save.
    inline constexpr std::size_t lz4_min_input = 32;

    // Returns the compressed block, or an empty vector when the input did not get smaller
    // (the caller then stores it verbatim, which is a legal choice per §15).
    std::vector<std::byte> lz4_compress(std::span<const std::byte> input);

    // Decodes exactly `output.size()` bytes. Any malformed sequence, a zero offset, a match
    // that reaches before the start of the block or a size mismatch is an error, never a
    // partially written buffer.
    std::expected<void, TrfError> lz4_decompress(std::span<const std::byte> input,
        std::span<std::byte> output, std::uint32_t chunk_id = no_chunk_id);

} // namespace terila::world::trf
