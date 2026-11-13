#pragma once

#include "world/trf/trf_error.hpp"
#include "world/trf/trf_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace terila::world::trf {

    // One of the two 64-byte halves of an index entry (§6).
    struct Slot {
        std::uint32_t record_offset = 0;
        std::uint32_t stored_size = 0;
        std::uint32_t raw_size = 0;
        std::uint8_t compression = 0;
        std::uint8_t flags = 0;
        std::uint8_t lx = 0;
        std::uint8_t lz = 0;
        std::uint32_t stored_crc32c = 0;
        std::uint32_t raw_crc32c = 0;
        std::uint32_t slot_generation = 0;

        void encode(std::span<std::byte, slot_size> out) const noexcept;

        // Returns nullopt when slot_crc32c does not match: such a slot is not valid at all.
        static std::optional<Slot> decode(std::span<const std::byte, slot_size> in) noexcept;

        bool addresses_record() const noexcept { return record_offset != 0; }
        bool deleted() const noexcept { return (flags & slot_flags::deleted) != 0; }
        bool damaged() const noexcept { return (flags & slot_flags::damaged) != 0; }
        bool edited() const noexcept { return (flags & slot_flags::edited) != 0; }
        bool wire_ready() const noexcept { return (flags & slot_flags::wire_ready) != 0; }
        bool light_full() const noexcept { return (flags & slot_flags::light_full) != 0; }

        // §16: the canonical tombstone references nothing and is therefore exempt from the
        // record bounds. It must be recognised before the codec rules, because compression 0
        // is deliberately not a codec.
        bool is_canonical_tombstone() const noexcept {
            return deleted() && record_offset == 0 && stored_size == 0 && raw_size == 0 &&
                compression == compression::none && stored_crc32c == 0 && raw_crc32c == 0;
        }

        // §21 bounds for one slot. chunk_id == no_chunk_id skips the coordinate check.
        std::expected<void, TrfError> validate(std::uint64_t file_size, std::uint32_t chunk_id) const;
    };

    std::optional<Slot> winning_slot(const std::optional<Slot>& slot_a,
        const std::optional<Slot>& slot_b) noexcept;

    std::optional<Slot> read_index_entry(std::span<const std::byte, index_entry_size> entry) noexcept;

    std::uint32_t next_slot_generation(const std::optional<Slot>& slot_a,
        const std::optional<Slot>& slot_b) noexcept;

}
