#include "world/trf/trf_lz4.hpp"

#include <algorithm>
#include <cstring>

namespace terila::world::trf {

    namespace {

        // LZ4 needs at least five literals at the end of the block, so a match may never
        // start or run into the last five bytes.
        constexpr std::size_t kLastLiterals = 5;
        constexpr std::uint32_t kHashBits = 14;
        constexpr std::uint32_t kHashSize = 1u << kHashBits;
        constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;
        constexpr std::size_t kMaxOffset = 0xFFFF;

        std::uint32_t load_u32(std::span<const std::byte> bytes, std::size_t at) {
            return static_cast<std::uint32_t>(bytes[at]) |
                (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
                (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
                (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
        }

        std::uint32_t hash4(std::uint32_t value) {
            return (value * 2654435761u) >> (32 - kHashBits);
        }

    } // namespace

    std::vector<std::byte> lz4_compress(std::span<const std::byte> input) {
        const std::size_t size = input.size();
        if (size < lz4_min_input) {
            return {};
        }

        std::vector<std::byte> out;
        out.reserve(size);

        std::vector<std::uint32_t> table(kHashSize, kEmpty);

        const auto emit_length = [&out](std::size_t length) {
            while (length >= 255) {
                out.push_back(std::byte{ 255 });
                length -= 255;
            }
            out.push_back(static_cast<std::byte>(length));
        };

        const auto emit_sequence = [&](std::size_t literal_at, std::size_t literal_len,
            std::size_t match_len, std::size_t offset) {
            const std::size_t token_literal = std::min<std::size_t>(literal_len, 15);
            const std::size_t extra_match = match_len - 4;
            const std::size_t token_match = std::min<std::size_t>(extra_match, 15);
            out.push_back(static_cast<std::byte>((token_literal << 4) | token_match));
            if (literal_len >= 15) {
                emit_length(literal_len - 15);
            }
            out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(literal_at),
                input.begin() + static_cast<std::ptrdiff_t>(literal_at + literal_len));
            out.push_back(static_cast<std::byte>(offset & 0xFF));
            out.push_back(static_cast<std::byte>((offset >> 8) & 0xFF));
            if (extra_match >= 15) {
                emit_length(extra_match - 15);
            }
        };

        const std::size_t match_limit = size - kLastLiterals;
        std::size_t anchor = 0;
        std::size_t pos = 0;

        while (pos + 4 <= match_limit) {
            const std::uint32_t value = load_u32(input, pos);
            const std::uint32_t slot = hash4(value);
            const std::uint32_t candidate = table[slot];
            table[slot] = static_cast<std::uint32_t>(pos);

            if (candidate == kEmpty || pos - candidate > kMaxOffset ||
                load_u32(input, candidate) != value) {
                ++pos;
                continue;
            }

            std::size_t match = 4;
            while (pos + match < match_limit && input[candidate + match] == input[pos + match]) {
                ++match;
            }

            emit_sequence(anchor, pos - anchor, match, pos - candidate);
            anchor = pos + match;
            pos = anchor;
        }

        // The trailing literals are a sequence of their own, without an offset.
        const std::size_t literal_len = size - anchor;
        out.push_back(static_cast<std::byte>(std::min<std::size_t>(literal_len, 15) << 4));
        if (literal_len >= 15) {
            emit_length(literal_len - 15);
        }
        out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(anchor), input.end());

        return out;
    }

    std::expected<void, TrfError> lz4_decompress(std::span<const std::byte> input,
        std::span<std::byte> output, std::uint32_t chunk_id) {

        const auto corrupt = [chunk_id](const char* detail) {
            return make_chunk_error(TrfErrorCode::CompressionFailed, chunk_id, detail);
        };

        std::size_t in = 0;
        std::size_t written = 0;

        while (in < input.size()) {
            const std::uint8_t token = static_cast<std::uint8_t>(input[in++]);

            std::size_t literal_len = token >> 4;
            if (literal_len == 15) {
                std::uint8_t extra = 0;
                do {
                    if (in >= input.size()) {
                        return std::unexpected(corrupt("truncated literal length"));
                    }
                    extra = static_cast<std::uint8_t>(input[in++]);
                    literal_len += extra;
                } while (extra == 255);
            }

            if (literal_len > input.size() - in || literal_len > output.size() - written) {
                return std::unexpected(corrupt("literal run leaves the block"));
            }
            std::memcpy(output.data() + written, input.data() + in, literal_len);
            in += literal_len;
            written += literal_len;

            // A block that ends here is the final literals-only sequence.
            if (in == input.size()) {
                break;
            }

            if (input.size() - in < 2) {
                return std::unexpected(corrupt("truncated match offset"));
            }
            const std::size_t offset = static_cast<std::size_t>(input[in]) |
                (static_cast<std::size_t>(input[in + 1]) << 8);
            in += 2;
            if (offset == 0 || offset > written) {
                return std::unexpected(corrupt("match offset is outside the block"));
            }

            std::size_t match_len = token & 0x0F;
            if (match_len == 15) {
                std::uint8_t extra = 0;
                do {
                    if (in >= input.size()) {
                        return std::unexpected(corrupt("truncated match length"));
                    }
                    extra = static_cast<std::uint8_t>(input[in++]);
                    match_len += extra;
                } while (extra == 255);
            }
            match_len += 4;

            if (match_len > output.size() - written) {
                return std::unexpected(corrupt("match run leaves the block"));
            }
            // Overlapping copies are how LZ4 encodes repeats, so this is byte by byte.
            std::byte* destination = output.data() + written;
            const std::byte* source = destination - offset;
            for (std::size_t i = 0; i < match_len; ++i) {
                destination[i] = source[i];
            }
            written += match_len;
        }

        if (written != output.size()) {
            return std::unexpected(corrupt("decoded size does not match raw_size"));
        }
        return {};
    }

} // namespace terila::world::trf
