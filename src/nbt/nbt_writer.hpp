#pragma once

#include "nbt/nbt_reader.hpp"
#include "protocol/primitives.hpp"
#include "protocol/varint.hpp"

#include <bit>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace terila::nbt {

class Writer {
public:
    explicit Writer(StringFormat format = StringFormat::Network) : format_(format) {}

    void begin_root_compound() {
        output_.push_back(static_cast<std::uint8_t>(Tag::Compound));
        if (format_ == StringFormat::Classic) write_string_raw("");
        ++compound_depth_;
    }
    void begin_compound(std::string_view name) { write_header(Tag::Compound, name); ++compound_depth_; }
    void end_compound() { if (compound_depth_ != 0) { output_.push_back(0); --compound_depth_; } }
    void end() { end_compound(); }

    void write_byte(std::string_view name, std::int8_t value) { write_header(Tag::Byte, name); output_.push_back(static_cast<std::uint8_t>(value)); }
    void write_short(std::string_view name, std::int16_t value) { write_header(Tag::Short, name); write_be(static_cast<std::uint16_t>(value)); }
    void write_int(std::string_view name, std::int32_t value) { write_header(Tag::Int, name); write_be(static_cast<std::uint32_t>(value)); }
    void write_long(std::string_view name, std::int64_t value) { write_header(Tag::Long, name); write_be(static_cast<std::uint64_t>(value)); }
    void write_float(std::string_view name, float value) { write_header(Tag::Float, name); write_be(std::bit_cast<std::uint32_t>(value)); }
    void write_double(std::string_view name, double value) { write_header(Tag::Double, name); write_be(std::bit_cast<std::uint64_t>(value)); }
    void write_string(std::string_view name, std::string_view value) { write_header(Tag::String, name); write_string_raw(value); }
    void write_value(const NbtValue& value) { write_payload(value); }
    void write_named_value(std::string_view name, const NbtValue& value) {
        if (!is_valid_value(value)) { failed_ = true; return; }
        write_header(value.tag(), name);
        write_payload(value);
    }
    std::vector<std::uint8_t> finish() { return failed_ ? std::vector<std::uint8_t>{} : std::move(output_); }

private:
    StringFormat format_;
    std::vector<std::uint8_t> output_;
    std::size_t compound_depth_{};
    bool failed_{};

    template<class U> void write_be(U value) {
        for (std::size_t i = 0; i < sizeof(U); ++i)
            output_.push_back(static_cast<std::uint8_t>(value >> ((sizeof(U) - i - 1) * 8)));
    }
    void write_string_raw(std::string_view value) {
        if (value.size() > (std::numeric_limits<std::uint16_t>::max)()) { failed_ = true; return; }
        terila::protocol::write_uint16(output_, static_cast<std::uint16_t>(value.size()));
        output_.insert(output_.end(), value.begin(), value.end());
    }
    void write_count(std::size_t count) {
        if (count > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) { failed_ = true; return; }
        terila::protocol::write_int32(output_, static_cast<std::int32_t>(count));
    }
    void write_header(Tag tag, std::string_view name) { output_.push_back(static_cast<std::uint8_t>(tag)); write_string_raw(name); }
    bool is_valid_value(const NbtValue& value) const {
        return std::visit([this](const auto& item) -> bool {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return string_size_valid(item.size());
            } else if constexpr (std::is_same_v<T, NbtList>) {
                if (item.empty()) return true;
                const Tag elem = item.front().tag();
                for (const auto& child : item) {
                    if (child.tag() != elem || !is_valid_value(child)) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, NbtCompound>) {
                for (const auto& [name, child] : item) {
                    if (!string_size_valid(name.size()) || !is_valid_value(child)) return false;
                }
                return true;
            } else {
                return true;
            }
        }, value.value);
    }
    bool string_size_valid(std::size_t size) const {
        return size <= (std::numeric_limits<std::uint16_t>::max)();
    }
    void write_payload(const NbtValue& value) {
        if (!is_valid_value(value)) { failed_ = true; return; }
        std::visit([this](const auto& item) { using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::int8_t>) output_.push_back(static_cast<std::uint8_t>(item));
            else if constexpr (std::is_same_v<T, std::int16_t>) write_be(static_cast<std::uint16_t>(item));
            else if constexpr (std::is_same_v<T, std::int32_t>) write_be(static_cast<std::uint32_t>(item));
            else if constexpr (std::is_same_v<T, std::int64_t>) write_be(static_cast<std::uint64_t>(item));
            else if constexpr (std::is_same_v<T, float>) write_be(std::bit_cast<std::uint32_t>(item));
            else if constexpr (std::is_same_v<T, double>) write_be(std::bit_cast<std::uint64_t>(item));
            else if constexpr (std::is_same_v<T, std::string>) write_string_raw(item);
            else if constexpr (std::is_same_v<T, NbtByteArray>) { write_count(item.size()); for (auto v : item) output_.push_back(static_cast<std::uint8_t>(v)); }
            else if constexpr (std::is_same_v<T, NbtIntArray>) { write_count(item.size()); for (auto v : item) write_be(static_cast<std::uint32_t>(v)); }
            else if constexpr (std::is_same_v<T, NbtLongArray>) { write_count(item.size()); for (auto v : item) write_be(static_cast<std::uint64_t>(v)); }
            else if constexpr (std::is_same_v<T, NbtCompound>) { for (const auto& [name, child] : item) write_named_value(name, child); output_.push_back(0); }
            else if constexpr (std::is_same_v<T, NbtList>) {
                const Tag elem = item.empty() ? Tag::End : item.front().tag();
                output_.push_back(static_cast<std::uint8_t>(elem)); write_count(item.size());
                for (const auto& child : item) write_payload(child);
            }
        }, value.value);
    }
};

}
