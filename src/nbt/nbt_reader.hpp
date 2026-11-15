#pragma once

#include "nbt/nbt_value.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace terila::nbt {

enum class StringFormat { Classic, Network };

struct NamedTag {
    Tag type;
    std::string name;
    NbtValue value;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data, StringFormat format = StringFormat::Network)
        : data_(data), format_(format) {}
    explicit Reader(const std::vector<std::uint8_t>& data, StringFormat format = StringFormat::Network)
        : Reader(std::span<const std::uint8_t>(data), format) {}

    std::size_t position() const noexcept { return pos_; }

    std::optional<NbtCompound> read_root_compound() {
        nodes_ = 0;
        const auto old = pos_;
        std::uint8_t raw{};
        if (!read_u8(raw) || raw != static_cast<std::uint8_t>(Tag::Compound)) return fail<NbtCompound>(old);
        if (format_ == StringFormat::Classic) {
            auto root_name = read_string_value();
            if (!root_name) return fail<NbtCompound>(old);
        }
        auto root = read_compound_payload();
        if (!root) return fail<NbtCompound>(old);
        return root;
    }

    std::optional<NamedTag> read_named_tag() {
        if (depth_ == 0) nodes_ = 0;
        const auto old = pos_;
        std::uint8_t raw{};
        if (!read_u8(raw) || raw == 0 || raw > 12) return fail<NamedTag>(old);
        auto name = read_string_value();
        if (!name) return fail<NamedTag>(old);
        auto value = read_payload(static_cast<Tag>(raw));
        if (!value) return fail<NamedTag>(old);
        return NamedTag{static_cast<Tag>(raw), std::move(*name), std::move(*value)};
    }

    std::optional<NbtValue> read_payload(Tag type) {
        const auto old = pos_;
        if (depth_ >= 256) { pos_ = old; return std::nullopt; }
        // One input byte can create a whole NbtValue, so a 16 MB list asked for gigabytes.
        // The budget bounds the total allocation, not the input size.
        if (++nodes_ > kMaxNodes) { pos_ = old; return std::nullopt; }
        struct DepthGuard { std::size_t& depth; explicit DepthGuard(std::size_t& value) : depth(value) { ++depth; } ~DepthGuard() { --depth; } } guard(depth_);
        if (type == Tag::End) return fail<NbtValue>(old);
        NbtValue result;
        std::uint8_t byte{};
        std::uint16_t short_value{};
        std::uint32_t int_value{};
        std::uint64_t long_value{};
        switch (type) {
        case Tag::Byte:
            if (!read_u8(byte)) return fail<NbtValue>(old);
            result = NbtValue(static_cast<std::int8_t>(byte)); break;
        case Tag::Short:
            if (!read_be(short_value)) return fail<NbtValue>(old);
            result = NbtValue(static_cast<std::int16_t>(short_value)); break;
        case Tag::Int:
            if (!read_be(int_value)) return fail<NbtValue>(old);
            result = NbtValue(static_cast<std::int32_t>(int_value)); break;
        case Tag::Long:
            if (!read_be(long_value)) return fail<NbtValue>(old);
            result = NbtValue(static_cast<std::int64_t>(long_value)); break;
        case Tag::Float:
            if (!read_be(int_value)) return fail<NbtValue>(old);
            result = NbtValue(std::bit_cast<float>(int_value)); break;
        case Tag::Double:
            if (!read_be(long_value)) return fail<NbtValue>(old);
            result = NbtValue(std::bit_cast<double>(long_value)); break;
        case Tag::String: {
            auto text = read_string_value(); if (!text) return fail<NbtValue>(old);
            result = NbtValue(std::move(*text)); break;
        }
        case Tag::ByteArray: {
            auto count = read_count(); if (!count || !has(*count)) return fail<NbtValue>(old);
            NbtByteArray values; values.reserve(*count);
            for (std::size_t i = 0; i < *count; ++i) {
                if (!read_u8(byte)) return fail<NbtValue>(old);
                values.push_back(static_cast<std::int8_t>(byte));
            }
            result = NbtValue(std::move(values)); break;
        }
        case Tag::IntArray: {
            auto count = read_count(); if (!count || *count > remaining() / 4) return fail<NbtValue>(old);
            NbtIntArray values; values.reserve(*count);
            for (std::size_t i = 0; i < *count; ++i) {
                if (!read_be(int_value)) return fail<NbtValue>(old);
                values.push_back(static_cast<std::int32_t>(int_value));
            }
            result = NbtValue(std::move(values)); break;
        }
        case Tag::LongArray: {
            auto count = read_count(); if (!count || *count > remaining() / 8) return fail<NbtValue>(old);
            NbtLongArray values; values.reserve(*count);
            for (std::size_t i = 0; i < *count; ++i) {
                if (!read_be(long_value)) return fail<NbtValue>(old);
                values.push_back(static_cast<std::int64_t>(long_value));
            }
            result = NbtValue(std::move(values)); break;
        }
        case Tag::List: {
            std::uint8_t elem_raw{}; if (!read_u8(elem_raw) || elem_raw > 12) return fail<NbtValue>(old);
            auto count = read_count(); if (!count || (elem_raw == 0 && *count != 0)) return fail<NbtValue>(old);
            if (*count > remaining()) return fail<NbtValue>(old);
            NbtList values; values.reserve(std::min(*count, kMaxNodes));
            for (std::size_t i = 0; i < *count; ++i) {
                auto item = read_payload(static_cast<Tag>(elem_raw)); if (!item) return fail<NbtValue>(old);
                values.push_back(std::move(*item));
            }
            result = NbtValue(std::move(values)); break;
        }
        case Tag::Compound: {
            auto value = read_compound_payload(); if (!value) return fail<NbtValue>(old);
            result = NbtValue(std::move(*value)); break;
        }
        default: return fail<NbtValue>(old);
        }
        return result;
    }

private:
    // 1M values is a few tens of megabytes at worst and far more than any real payload.
    static constexpr std::size_t kMaxNodes = 1'000'000;

    std::span<const std::uint8_t> data_;
    StringFormat format_;
    std::size_t pos_{};
    std::size_t depth_{};
    std::size_t nodes_{};

    std::size_t remaining() const noexcept { return data_.size() - pos_; }
    bool has(std::size_t count) const noexcept { return count <= remaining(); }
    bool read_u8(std::uint8_t& value) { if (!has(1)) return false; value = data_[pos_++]; return true; }
    template<class U> bool read_be(U& value) {
        if (!has(sizeof(U))) return false;
        value = 0;
        for (std::size_t i = 0; i < sizeof(U); ++i) value = static_cast<U>((value << 8) | data_[pos_++]);
        return true;
    }
    std::optional<std::size_t> read_count() {
        std::uint32_t raw{};
        if (!read_be(raw)) return std::nullopt;
        const auto count = static_cast<std::int32_t>(raw);
        if (count < 0) return std::nullopt;
        return static_cast<std::size_t>(count);
    }
    std::optional<std::string> read_string_value() {
        std::uint16_t raw{};
        if (!read_be(raw)) return std::nullopt;
        const std::size_t length = raw;
        if (!has(length)) return std::nullopt;
        std::string value(reinterpret_cast<const char*>(data_.data() + pos_), length);
        pos_ += length; return value;
    }
    std::optional<NbtCompound> read_compound_payload() {
        NbtCompound values;
        while (true) {
            if (!has(1)) return std::nullopt;
            if (data_[pos_] == 0) { ++pos_; return values; }
            auto named = read_named_tag(); if (!named) return std::nullopt;
            if (!values.emplace(std::move(named->name), std::move(named->value)).second) return std::nullopt;
        }
    }
    template<class T> std::optional<T> fail(std::size_t old) { pos_ = old; return std::nullopt; }
};

}
