#pragma once

// A tag tree: the small, self describing data format the game speaks. One variant holds
// every shape, so walking a tree is a visit and not a class hierarchy; the arrays and the
// two containers keep their own vectors, which is what the binary form counts.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace terila::nbt {

    enum class Tag : std::uint8_t {
        End = 0, Byte = 1, Short = 2, Int = 3, Long = 4, Float = 5,
        Double = 6, Bytes = 7, Text = 8, List = 9, Map = 10,
        Ints = 11, Longs = 12
    };

    struct NbtValue;
    using NbtList = std::vector<NbtValue>;
    using NbtCompound = std::map<std::string, NbtValue>;
    using NbtBytes = std::vector<std::uint8_t>;
    using NbtInts = std::vector<std::int32_t>;
    using NbtLongs = std::vector<std::int64_t>;

    // A value is one of the twelve shapes; nothing is ever empty-typed by accident, so
    // the tag index maps straight onto the id the binary form uses.
    struct NbtValue {
        using Storage = std::variant<std::int8_t, std::int16_t, std::int32_t, std::int64_t,
            float, double, NbtBytes, std::string, NbtList, NbtCompound, NbtInts, NbtLongs>;

        Storage value;

        NbtValue() : value(NbtCompound{}) {}
        NbtValue(std::int8_t item) : value(item) {}
        NbtValue(std::int16_t item) : value(item) {}
        NbtValue(std::int32_t item) : value(item) {}
        NbtValue(std::int64_t item) : value(item) {}
        NbtValue(float item) : value(item) {}
        NbtValue(double item) : value(item) {}
        NbtValue(std::string item) : value(std::move(item)) {}
        NbtValue(const char* item) : value(std::string(item)) {}
        NbtValue(NbtBytes item) : value(std::move(item)) {}
        NbtValue(NbtInts item) : value(std::move(item)) {}
        NbtValue(NbtLongs item) : value(std::move(item)) {}
        NbtValue(NbtList item) : value(std::move(item)) {}
        NbtValue(NbtCompound item) : value(std::move(item)) {}

        // An unsigned int would silently pick one of the signed widths; that is a bug at
        // every call site, so it is not offered at all.
        NbtValue(unsigned int) = delete;

        template<class T> T* get_if() noexcept { return std::get_if<T>(&value); }
        template<class T> const T* get_if() const noexcept { return std::get_if<T>(&value); }

        Tag tag() const noexcept { return static_cast<Tag>(value.index() + 1); }

        bool operator==(const NbtValue&) const = default;

        // Numbers convert the way the game truncates them, which is what a reader that
        // accepts "any number here" needs.
        std::optional<double> as_double() const {
            if (const auto* v = get_if<std::int8_t>()) return static_cast<double>(*v);
            if (const auto* v = get_if<std::int16_t>()) return static_cast<double>(*v);
            if (const auto* v = get_if<std::int32_t>()) return static_cast<double>(*v);
            if (const auto* v = get_if<std::int64_t>()) return static_cast<double>(*v);
            if (const auto* v = get_if<float>()) return static_cast<double>(*v);
            if (const auto* v = get_if<double>()) return *v;
            return std::nullopt;
        }

        std::optional<std::int64_t> as_long() const {
            if (const auto number = as_double()) {
                return static_cast<std::int64_t>(*number);
            }
            return std::nullopt;
        }

        std::optional<std::string_view> as_text() const {
            if (const auto* v = get_if<std::string>()) {
                return std::string_view(*v);
            }
            return std::nullopt;
        }
    };

    // Human readable name of a tag type: for logs, error messages and the tree dump.
    inline std::string_view tag_name(Tag kind) noexcept {
        switch (kind) {
        case Tag::End: return "end";
        case Tag::Byte: return "byte";
        case Tag::Short: return "short";
        case Tag::Int: return "int";
        case Tag::Long: return "long";
        case Tag::Float: return "float";
        case Tag::Double: return "double";
        case Tag::Bytes: return "bytes";
        case Tag::Text: return "text";
        case Tag::List: return "list";
        case Tag::Map: return "map";
        case Tag::Ints: return "ints";
        case Tag::Longs: return "longs";
        }
        return "unknown";
    }

    // One child of a map before it is stored: the binary and text forms both need the name
    // and the value next to each other.
    struct NamedTag {
        Tag kind = Tag::End;
        std::string name;
        NbtValue value;
    };

    namespace detail {

        // A map holds children under their name; the helpers below keep the call sites from
        // spelling out the variant every time.
        inline const NbtValue* map_get(const NbtCompound& map, std::string_view name) {
            const auto it = map.find(std::string(name));
            return it == map.end() ? nullptr : &it->second;
        }

        inline NbtValue* map_get(NbtCompound& map, std::string_view name) {
            const auto it = map.find(std::string(name));
            return it == map.end() ? nullptr : &it->second;
        }

        inline const NbtList* list_at(const NbtValue& value) {
            return value.get_if<NbtList>();
        }

        // A list must stay homogeneous: the element type of an empty list is decided by the
        // first value pushed into it, and mixing types afterwards is refused.
        inline bool list_accepts(const NbtList& list, Tag kind) {
            return list.empty() || list.front().tag() == kind;
        }

    } // namespace detail

    // The four helpers most call sites need, written as functions so a value stays a plain
    // struct instead of growing fifty methods.
    inline const NbtValue* at(const NbtValue& value, std::string_view name) {
        if (const auto* map = value.get_if<NbtCompound>()) {
            return detail::map_get(*map, name);
        }
        return nullptr;
    }

    inline std::optional<std::int32_t> int_at(const NbtValue& value, std::string_view name) {
        const auto* child = at(value, name);
        if (child == nullptr) {
            return std::nullopt;
        }
        return child->as_long().has_value()
            ? std::optional<std::int32_t>(static_cast<std::int32_t>(*child->as_long()))
            : std::nullopt;
    }

    inline std::optional<std::string_view> text_at(const NbtValue& value, std::string_view name) {
        const auto* child = at(value, name);
        return child == nullptr ? std::nullopt : child->as_text();
    }

    inline bool has(const NbtValue& value, std::string_view name) {
        return at(value, name) != nullptr;
    }

} // namespace terila::nbt
