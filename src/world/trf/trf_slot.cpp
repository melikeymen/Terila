#include "world/trf/trf_slot.hpp"

#include "world/trf/trf_crc32c.hpp"

#include <algorithm>

namespace terila::world::trf {

    void Slot::encode(std::span<std::byte, slot_size> out) const noexcept {
        std::fill(out.begin(), out.end(), std::byte{ 0 });
        store_u32(out.data() + 0x00, record_offset);
        store_u32(out.data() + 0x04, stored_size);
        store_u32(out.data() + 0x08, raw_size);
        out[0x0C] = static_cast<std::byte>(compression);
        out[0x0D] = static_cast<std::byte>(flags);
        out[0x0E] = static_cast<std::byte>(lx);
        out[0x0F] = static_cast<std::byte>(lz);
        store_u32(out.data() + 0x10, stored_crc32c);
        store_u32(out.data() + 0x14, raw_crc32c);
        store_u32(out.data() + 0x18, slot_generation);
        store_u32(out.data() + slot_crc_offset, crc32c(out.first(slot_crc_coverage)));
    }

    std::optional<Slot> Slot::decode(std::span<const std::byte, slot_size> in) noexcept {
        const std::uint32_t stored_crc = load_u32(in.data() + slot_crc_offset);
        if (crc32c(in.first(slot_crc_coverage)) != stored_crc) {
            return std::nullopt;
        }

        Slot slot;
        slot.record_offset = load_u32(in.data() + 0x00);
        slot.stored_size = load_u32(in.data() + 0x04);
        slot.raw_size = load_u32(in.data() + 0x08);
        slot.compression = static_cast<std::uint8_t>(in[0x0C]);
        slot.flags = static_cast<std::uint8_t>(in[0x0D]);
        slot.lx = static_cast<std::uint8_t>(in[0x0E]);
        slot.lz = static_cast<std::uint8_t>(in[0x0F]);
        slot.stored_crc32c = load_u32(in.data() + 0x10);
        slot.raw_crc32c = load_u32(in.data() + 0x14);
        slot.slot_generation = load_u32(in.data() + 0x18);
        return slot;
    }

    std::expected<void, TrfError> Slot::validate(std::uint64_t file_size,
        std::uint32_t chunk_id_value) const {

        if ((flags & slot_flags::reserved_mask) != 0) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                "slot uses reserved flag bits"));
        }

        // A slot that names no record is legal only in canonical tombstone form, and that
        // form is tested before every codec/size rule (§21).
        if (!addresses_record()) {
            if (!is_canonical_tombstone()) {
                return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                    "slot references no record but is not a canonical tombstone"));
            }
            return {};
        }

        if (chunk_id_value != no_chunk_id) {
            const auto expected_lx = static_cast<std::uint8_t>(chunk_id_value & 31);
            const auto expected_lz = static_cast<std::uint8_t>((chunk_id_value >> 5) & 31);
            if (lx != expected_lx || lz != expected_lz) {
                return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                    "slot coordinates do not match its index id"));
            }
        }

        if (raw_size == 0 || raw_size > max_raw_size) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                "raw_size out of bounds"));
        }
        if (stored_size == 0) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                "stored_size is zero for a slot that names a record"));
        }
        if (compression < compression::first_codec || compression > compression::last_codec) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                "unknown compression codec"));
        }
        if (compression == compression::stored) {
            if (stored_size != raw_size + stored_framing_overhead) {
                return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                    "Stored record size does not match raw size"));
            }
        }
        else {
            if (stored_size < min_compressed_stored_size) {
                return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                    "compressed record smaller than its prefix"));
            }
            if (stored_size >= raw_size + stored_framing_overhead) {
                return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                    "compressed record was not compressed"));
            }
        }
        if (stored_size >= raw_size && compression != compression::stored) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                "stored_size >= raw_size requires the Stored codec"));
        }
        if (record_offset % data_align != 0) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                "record_offset is not 8-byte aligned"));
        }
        if (record_offset < data_start) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                "record_offset falls inside the header or the index"));
        }

        const std::uint64_t end = static_cast<std::uint64_t>(record_offset) + stored_size;
        const std::uint64_t data_end_limit =
            file_size > footer_size ? file_size - footer_size : 0;
        if (end > data_end_limit) {
            return std::unexpected(make_chunk_error(TrfErrorCode::CorruptHeader, chunk_id_value,
                "record range falls outside the data region"));
        }

        return {};
    }

    std::optional<Slot> winning_slot(const std::optional<Slot>& slot_a,
        const std::optional<Slot>& slot_b) noexcept {
        if (!slot_a) {
            return slot_b;
        }
        if (!slot_b) {
            return slot_a;
        }
        if (slot_b->slot_generation > slot_a->slot_generation) {
            return slot_b;
        }
        return slot_a;
    }

    std::optional<Slot> read_index_entry(std::span<const std::byte, index_entry_size> entry) noexcept {
        const auto slot_a = Slot::decode(entry.first<slot_size>());
        const auto slot_b = Slot::decode(entry.subspan<slot_size, slot_size>());
        return winning_slot(slot_a, slot_b);
    }

    std::uint32_t next_slot_generation(const std::optional<Slot>& slot_a,
        const std::optional<Slot>& slot_b) noexcept {
        const std::uint32_t gen_a = slot_a ? slot_a->slot_generation : 0;
        const std::uint32_t gen_b = slot_b ? slot_b->slot_generation : 0;
        return std::max(gen_a, gen_b) + 1;
    }

}
