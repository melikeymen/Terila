#pragma once

#include "nbt/nbt_value.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace terila::nbt {

    // The text form NBT takes inside a command: /give diamond_sword[minecraft:custom_name='"Hi"']
    // carries its component values as stringified NBT, so the arguments are read here and
    // written back out in the binary form the client decodes.
    class SnbtParser {
    public:
        explicit SnbtParser(std::string_view text) : text_(text) {}

        std::optional<NbtValue> parse() {
            skip_space();
            auto value = parse_value();
            if (!value) {
                return std::nullopt;
            }
            skip_space();
            if (pos_ != text_.size()) {
                return std::nullopt;
            }
            return value;
        }

    private:
        // Commands nest a handful of levels at most; recursion still needs a bound.
        static constexpr std::size_t kMaxDepth = 32;

        std::string_view text_;
        std::size_t pos_{};
        std::size_t depth_{};

        bool has() const { return pos_ < text_.size(); }
        char peek() const { return text_[pos_]; }
        bool eat(char expected) {
            if (has() && text_[pos_] == expected) {
                ++pos_;
                return true;
            }
            return false;
        }
        void skip_space() {
            while (has() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }

        std::optional<NbtValue> parse_value() {
            if (depth_ >= kMaxDepth) {
                return std::nullopt;
            }
            skip_space();
            if (!has()) {
                return std::nullopt;
            }
            ++depth_;
            std::optional<NbtValue> value;
            switch (peek()) {
            case '{': value = parse_compound(); break;
            case '[': value = parse_list(); break;
            case '"':
            case '\'': value = parse_quoted(); break;
            default: value = parse_bare(); break;
            }
            --depth_;
            return value;
        }

        std::optional<NbtValue> parse_compound() {
            if (!eat('{')) {
                return std::nullopt;
            }
            NbtCompound values;
            skip_space();
            if (eat('}')) {
                return NbtValue(std::move(values));
            }
            while (true) {
                skip_space();
                auto key = parse_key();
                if (!key) {
                    return std::nullopt;
                }
                skip_space();
                if (!eat(':')) {
                    return std::nullopt;
                }
                auto value = parse_value();
                if (!value) {
                    return std::nullopt;
                }
                values.insert_or_assign(std::move(*key), std::move(*value));
                skip_space();
                if (eat(',')) {
                    skip_space();
                    if (eat('}')) {
                        return NbtValue(std::move(values));
                    }
                    continue;
                }
                if (eat('}')) {
                    return NbtValue(std::move(values));
                }
                return std::nullopt;
            }
        }

        std::optional<std::string> parse_key() {
            if (!has()) {
                return std::nullopt;
            }
            if (peek() == '"' || peek() == '\'') {
                auto text = parse_quoted();
                if (!text) {
                    return std::nullopt;
                }
                auto* string = text->get_if<std::string>();
                return string == nullptr ? std::nullopt : std::optional<std::string>(*string);
            }
            const std::size_t start = pos_;
            while (has() && peek() != ':' && std::isspace(static_cast<unsigned char>(peek())) == 0) {
                ++pos_;
            }
            if (pos_ == start) {
                return std::nullopt;
            }
            return std::string(text_.substr(start, pos_ - start));
        }

        std::optional<NbtValue> parse_list() {
            if (!eat('[')) {
                return std::nullopt;
            }
            skip_space();

            // [B;1b,2b], [I;1,2] and [L;1L,2L] are the array forms.
            if (has() && pos_ + 1 < text_.size() && text_[pos_ + 1] == ';') {
                const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(peek())));
                if (kind == 'B' || kind == 'I' || kind == 'L') {
                    pos_ += 2;
                    return parse_array(kind);
                }
            }
            if (eat(']')) {
                return NbtValue(NbtList{});
            }

            NbtList values;
            while (true) {
                auto value = parse_value();
                if (!value) {
                    return std::nullopt;
                }
                if (!values.empty() && values.front().tag() != value->tag()) {
                    return std::nullopt;   // NBT lists hold a single element type
                }
                values.push_back(std::move(*value));
                skip_space();
                if (eat(',')) {
                    skip_space();
                    if (eat(']')) {
                        return NbtValue(std::move(values));
                    }
                    continue;
                }
                if (eat(']')) {
                    return NbtValue(std::move(values));
                }
                return std::nullopt;
            }
        }

        std::optional<NbtValue> parse_array(char kind) {
            NbtByteArray bytes;
            NbtIntArray ints;
            NbtLongArray longs;
            skip_space();
            if (eat(']')) {
                if (kind == 'B') return NbtValue(std::move(bytes));
                if (kind == 'I') return NbtValue(std::move(ints));
                return NbtValue(std::move(longs));
            }
            while (true) {
                skip_space();
                auto value = parse_value();
                if (!value) {
                    return std::nullopt;
                }
                if (kind == 'B') {
                    auto* item = value->get_if<std::int8_t>();
                    if (item == nullptr) return std::nullopt;
                    bytes.push_back(*item);
                }
                else if (kind == 'I') {
                    auto* item = value->get_if<std::int32_t>();
                    if (item == nullptr) return std::nullopt;
                    ints.push_back(*item);
                }
                else {
                    auto* item = value->get_if<std::int64_t>();
                    if (item == nullptr) return std::nullopt;
                    longs.push_back(*item);
                }
                skip_space();
                if (eat(',')) {
                    skip_space();
                    if (eat(']')) break;
                    continue;
                }
                if (eat(']')) break;
                return std::nullopt;
            }
            if (kind == 'B') return NbtValue(std::move(bytes));
            if (kind == 'I') return NbtValue(std::move(ints));
            return NbtValue(std::move(longs));
        }

        std::optional<NbtValue> parse_quoted() {
            const char quote = peek();
            ++pos_;
            std::string text;
            while (has()) {
                const char c = text_[pos_++];
                if (c == quote) {
                    return NbtValue(std::move(text));
                }
                if (c != '\\') {
                    text.push_back(c);
                    continue;
                }
                if (!has()) {
                    return std::nullopt;
                }
                const char escaped = text_[pos_++];
                switch (escaped) {
                case 'n': text.push_back('\n'); break;
                case 't': text.push_back('\t'); break;
                case 'r': text.push_back('\r'); break;
                case '\\': text.push_back('\\'); break;
                case '"': text.push_back('"'); break;
                case '\'': text.push_back('\''); break;
                default: text.push_back(escaped); break;
                }
            }
            return std::nullopt;
        }

        // Bare words are numbers when they look like one and strings otherwise, which is
        // how /give diamond_sword[minecraft:unbreakable={}] and levels:{...:5} both read.
        std::optional<NbtValue> parse_bare() {
            const std::size_t start = pos_;
            while (has()) {
                const char c = peek();
                // ':' is not a terminator: a bare value such as minecraft:efficiency is one
                // word, while the key side stops at ':' in parse_key.
                if (c == ',' || c == ']' || c == '}' || c == '{' || c == '[' ||
                    std::isspace(static_cast<unsigned char>(c)) != 0) {
                    break;
                }
                ++pos_;
            }
            if (pos_ == start) {
                return std::nullopt;
            }
            const std::string token(text_.substr(start, pos_ - start));
            if (token == "true") return NbtValue(static_cast<std::int8_t>(1));
            if (token == "false") return NbtValue(static_cast<std::int8_t>(0));

            // A bare word that does not parse as a number is a string, exactly how the game
            // reads an unquoted token.
            if (auto parsed = number(token)) {
                return parsed;
            }
            return NbtValue(token);
        }

        std::optional<NbtValue> number(const std::string& token) {
            if (token.empty()) {
                return std::nullopt;
            }
            char suffix = token.back();
            const bool has_suffix = !std::isdigit(static_cast<unsigned char>(suffix)) && suffix != '.';
            const std::string digits = has_suffix ? token.substr(0, token.size() - 1) : token;
            if (digits.empty()) {
                return std::nullopt;
            }

            if (has_suffix) {
                switch (std::tolower(static_cast<unsigned char>(suffix))) {
                case 'b': {
                    const auto parsed = parse_integer<std::int8_t>(digits);
                    if (parsed) return NbtValue(*parsed);
                    break;
                }
                case 's': {
                    const auto parsed = parse_integer<std::int16_t>(digits);
                    if (parsed) return NbtValue(*parsed);
                    break;
                }
                case 'l': {
                    const auto parsed = parse_integer<std::int64_t>(digits);
                    if (parsed) return NbtValue(*parsed);
                    break;
                }
                case 'f': {
                    const auto parsed = parse_real(digits);
                    if (parsed) return NbtValue(static_cast<float>(*parsed));
                    break;
                }
                case 'd': {
                    const auto parsed = parse_real(digits);
                    if (parsed) return NbtValue(*parsed);
                    break;
                }
                default: break;
                }
                return NbtValue(token);   // 3x is a string, not a broken number
            }

            // No suffix: an integer that fits in 32 bits stays an int, a wider one becomes
            // a long (as the game does), and anything with a dot or exponent is a double.
            const bool real = token.find('.') != std::string::npos ||
                token.find('e') != std::string::npos || token.find('E') != std::string::npos;
            if (real) {
                const auto parsed = parse_real(token);
                if (!parsed) return NbtValue(token);
                return NbtValue(*parsed);
            }
            const auto as_int = parse_integer<std::int32_t>(token);
            if (as_int) {
                return NbtValue(*as_int);
            }
            const auto as_long = parse_integer<std::int64_t>(token);
            if (as_long) {
                return NbtValue(*as_long);
            }
            return NbtValue(token);
        }

        template<class T> std::optional<T> parse_integer(const std::string& digits) {
            std::size_t used = 0;
            long long value = 0;
            try {
                value = std::stoll(digits, &used);
            }
            catch (...) {
                return std::nullopt;
            }
            if (used != digits.size()) {
                return std::nullopt;
            }
            if (value < static_cast<long long>((std::numeric_limits<T>::min)()) ||
                value > static_cast<long long>((std::numeric_limits<T>::max)())) {
                return std::nullopt;
            }
            return static_cast<T>(value);
        }

        static std::optional<double> parse_real(const std::string& digits) {
            std::size_t used = 0;
            double value = 0.0;
            try {
                value = std::stod(digits, &used);
            }
            catch (...) {
                return std::nullopt;
            }
            if (used != digits.size()) {
                return std::nullopt;
            }
            return value;
        }
    };

    inline std::optional<NbtValue> parse_snbt(std::string_view text) {
        return SnbtParser(text).parse();
    }

} // namespace terila::nbt
