#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>
#include <utility>

namespace terila::nbt {

enum class Tag : std::uint8_t {
    End = 0, Byte = 1, Short = 2, Int = 3, Long = 4, Float = 5,
    Double = 6, ByteArray = 7, String = 8, List = 9, Compound = 10,
    IntArray = 11, LongArray = 12
};

struct NbtValue;
using NbtList = std::vector<NbtValue>;
using NbtCompound = std::map<std::string, NbtValue>;
using NbtByteArray = std::vector<std::int8_t>;
using NbtIntArray = std::vector<std::int32_t>;
using NbtLongArray = std::vector<std::int64_t>;

struct NbtValue {
    using Storage = std::variant<std::int8_t, std::int16_t, std::int32_t,
        std::int64_t, float, double, NbtByteArray, std::string, NbtList,
        NbtCompound, NbtIntArray, NbtLongArray>;
    Storage value;

    NbtValue() : value(NbtCompound{}) {}
    NbtValue(std::int8_t item) : value(item) {}
    NbtValue(std::int16_t item) : value(item) {}
    NbtValue(std::int32_t item) : value(item) {}
    NbtValue(std::int64_t item) : value(item) {}
    NbtValue(float item) : value(item) {}
    NbtValue(double item) : value(item) {}
    NbtValue(std::string item) : value(std::move(item)) {}
    NbtValue(NbtByteArray item) : value(std::move(item)) {}
    NbtValue(NbtIntArray item) : value(std::move(item)) {}
    NbtValue(NbtLongArray item) : value(std::move(item)) {}
    NbtValue(NbtList item) : value(std::move(item)) {}
    NbtValue(NbtCompound item) : value(std::move(item)) {}
    NbtValue(unsigned int) = delete;
    template<class T> T* get_if() noexcept { return std::get_if<T>(&value); }
    template<class T> const T* get_if() const noexcept { return std::get_if<T>(&value); }
    Tag tag() const noexcept {
        return static_cast<Tag>(value.index() + 1);
    }
    bool operator==(const NbtValue&) const = default;
};

}
