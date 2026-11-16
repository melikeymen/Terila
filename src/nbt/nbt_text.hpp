#pragma once

// The text form of a tag tree: how a command writes a value and how the server prints it
// back. Reading and writing live together here because both are about the same grammar —
// a number keeps its width in a suffix, a list keeps one element type, and a name is
// quoted only when it would otherwise read back as something else.

#include "nbt/nbt_codec.hpp"
#include "nbt/nbt_value.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terila::nbt {

    // ---- writing text ------------------------------------------------------------

    namespace text {

        inline bool plain_name(std::string_view name) {
            if (name.empty()) {
                return false;
            }
            const auto equals_ignoring_case = [&](std::string_view word) {
                if (name.size() != word.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < name.size(); ++i) {
                    const char c = name[i] >= 'A' && name[i] <= 'Z'
                        ? static_cast<char>(name[i] - 'A' + 'a')
                        : name[i];
                    if (c != word[i]) {
                        return false;
                    }
                }
                return true;
            };
            // "true" and "false" would come back as a boolean, so they always get quotes.
            if (equals_ignoring_case("true") || equals_ignoring_case("false")) {
                return false;
            }
            const auto head = name.front();
            if (!((head >= 'A' && head <= 'Z') || (head >= 'a' && head <= 'z') ||
                    head == '_' || head == '.')) {
                return false;
            }
            return std::all_of(name.begin() + 1, name.end(), [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '+' || c == '-';
            });
        }

        inline const char* control_letter(char c) {
            switch (c) {
            case '\t': return "t";
            case '\n': return "n";
            case '\r': return "r";
            case '\b': return "b";
            case '\f': return "f";
            default: return nullptr;
            }
        }

        // Whichever quote the text does not contain becomes the delimiter, so a name with an
        // apostrophe still prints without escaping half of its characters.
        inline void quoted(std::string& out, std::string_view text) {
            char quote = text.find('"') == std::string_view::npos ? '"' : '\'';
            if (quote == '\'' && text.find('\'') != std::string_view::npos) {
                quote = '"';
            }
            out.push_back(quote);
            for (const char c : text) {
                if (c == '\\') {
                    out += "\\\\";
                }
                else if (const char* letter = control_letter(c); letter != nullptr) {
                    out.push_back('\\');
                    out += letter;
                }
                else if (c == quote) {
                    out.push_back('\\');
                    out.push_back(c);
                }
                else {
                    out.push_back(c);
                }
            }
            out.push_back(quote);
        }

        inline void name(std::string& out, std::string_view text) {
            if (plain_name(text)) {
                out += text;
            }
            else {
                quoted(out, text);
            }
        }

        // %g has no digits to print for these, and "nan"/"inf" would read back as text, so
        // the three special values are spelled the way the reader accepts them.
        inline std::string real(float value) {
            if (std::isnan(value)) {
                return "NaN";
            }
            if (std::isinf(value)) {
                return value < 0.0f ? "-Infinity" : "Infinity";
            }
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
            return buffer;
        }

        inline std::string real(double value) {
            if (std::isnan(value)) {
                return "NaN";
            }
            if (std::isinf(value)) {
                return value < 0.0 ? "-Infinity" : "Infinity";
            }
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", value);
            return buffer;
        }

        inline void break_line(std::string& out, bool indented, int depth) {
            if (!indented) {
                return;
            }
            out.push_back('\n');
            out.append(static_cast<std::size_t>(depth) * 2, ' ');
        }

        inline void write(std::string& out, const NbtValue& value, bool indented, int depth) {
            if (const auto* v = value.get_if<std::int8_t>()) {
                out += std::to_string(static_cast<int>(*v));
                out.push_back('b');
            }
            else if (const auto* v = value.get_if<std::int16_t>()) {
                out += std::to_string(*v);
                out.push_back('s');
            }
            else if (const auto* v = value.get_if<std::int32_t>()) {
                out += std::to_string(*v);
            }
            else if (const auto* v = value.get_if<std::int64_t>()) {
                out += std::to_string(*v);
                out.push_back('L');
            }
            else if (const auto* v = value.get_if<float>()) {
                out += real(*v);
                out.push_back('f');
            }
            else if (const auto* v = value.get_if<double>()) {
                out += real(*v);
                out.push_back('d');
            }
            else if (const auto* v = value.get_if<std::string>()) {
                quoted(out, *v);
            }
            else if (const auto* v = value.get_if<NbtBytes>()) {
                out += "[B;";
                for (std::size_t i = 0; i < v->size(); ++i) {
                    if (i != 0) out.push_back(',');
                    if (indented) out.push_back(' ');
                    // Bytes are printed signed, which is what the reader accepts and what
                    // the game's own dump writes.
                    out += std::to_string(static_cast<int>(
                        static_cast<std::int8_t>((*v)[i])));
                    out.push_back('B');
                }
                out.push_back(']');
            }
            else if (const auto* v = value.get_if<NbtInts>()) {
                out += "[I;";
                for (std::size_t i = 0; i < v->size(); ++i) {
                    if (i != 0) out.push_back(',');
                    if (indented) out.push_back(' ');
                    out += std::to_string((*v)[i]);
                }
                out.push_back(']');
            }
            else if (const auto* v = value.get_if<NbtLongs>()) {
                out += "[L;";
                for (std::size_t i = 0; i < v->size(); ++i) {
                    if (i != 0) out.push_back(',');
                    if (indented) out.push_back(' ');
                    out += std::to_string((*v)[i]);
                    out.push_back('L');
                }
                out.push_back(']');
            }
            else if (const auto* v = value.get_if<NbtList>()) {
                out.push_back('[');
                for (std::size_t i = 0; i < v->size(); ++i) {
                    if (i != 0) out.push_back(',');
                    break_line(out, indented, depth + 1);
                    write(out, (*v)[i], indented, depth + 1);
                }
                if (!v->empty()) break_line(out, indented, depth);
                out.push_back(']');
            }
            else if (const auto* v = value.get_if<NbtCompound>()) {
                out.push_back('{');
                bool first = true;
                for (const auto& [key, child] : *v) {
                    if (!first) out.push_back(',');
                    first = false;
                    break_line(out, indented, depth + 1);
                    name(out, key);
                    out.push_back(':');
                    if (indented) out.push_back(' ');
                    write(out, child, indented, depth + 1);
                }
                if (!v->empty()) break_line(out, indented, depth);
                out.push_back('}');
            }
        }

    } // namespace text

    // One line, the form a command accepts back.
    inline std::string to_snbt(const NbtValue& value) {
        std::string out;
        text::write(out, value, false, 0);
        return out;
    }

    // The indented form the game's data dump uses.
    inline std::string to_snbt_pretty(const NbtValue& value) {
        std::string out;
        text::write(out, value, true, 0);
        return out;
    }

    // One line per entry with the type of its value: the shape of a tree, for logs.
    inline std::vector<std::string> describe_tree(const NbtValue& value) {
        std::vector<std::string> lines;
        std::function<void(const NbtValue&, int)> walk = [&](const NbtValue& node, int depth) {
            const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
            const auto descend = [&](const NbtValue& child) {
                if (child.get_if<NbtCompound>() != nullptr || child.get_if<NbtList>() != nullptr) {
                    walk(child, depth + 1);
                }
            };
            if (const auto* map = node.get_if<NbtCompound>()) {
                for (const auto& [key, child] : *map) {
                    lines.push_back(indent + key + ": " + std::string(tag_name(child.tag())));
                    descend(child);
                }
                return;
            }
            if (const auto* list = node.get_if<NbtList>()) {
                for (const auto& child : *list) {
                    lines.push_back(indent + "- " + std::string(tag_name(child.tag())));
                    descend(child);
                }
            }
        };
        walk(value, 0);
        return lines;
    }

    // A chat component is a map with a "text" entry, written as a standalone value.
    inline std::vector<std::uint8_t> text_component_nbt(std::string_view message) {
        NbtCompound component;
        component.insert_or_assign("text", NbtValue(std::string(message)));
        return encode_nbt(NbtValue(std::move(component)));
    }

    // ---- reading text ------------------------------------------------------------

    class SnbtReader {
    public:
        // Text carries no length in front of each value, so the budget is charged per node
        // instead of per byte: a megabyte of "{}" would otherwise expand many times its own
        // size in variants, exactly like the binary form.
        explicit SnbtReader(std::string_view input,
            ReadBudget budget = ReadBudget::from_client())
            : input_(input), budget_(std::move(budget)) {}

        // Nesting is recursion here too, so the same depth ceiling applies as in the
        // binary reader; text written by a player is not trustworthy either.
        static constexpr std::size_t kMaxDepth = 64;

        // Reads one value and insists that nothing is left over.
        NbtValue read_all() {
            auto value = read_value(false);
            skip_space();
            if (cursor_ < input_.size()) {
                throw BudgetExceeded("trailing data after the tag");
            }
            return value;
        }

    private:
        std::string_view input_;
        ReadBudget budget_;
        std::size_t cursor_ = 0;
        std::size_t depth_ = 0;

        struct Nest {
            explicit Nest(SnbtReader& reader) : reader_(reader) {
                if (++reader_.depth_ > kMaxDepth) {
                    reader_.fail("tag is nested too deeply");
                }
            }
            ~Nest() { --reader_.depth_; }
            Nest(const Nest&) = delete;
            Nest& operator=(const Nest&) = delete;

        private:
            SnbtReader& reader_;
        };

        [[noreturn]] void fail(std::string message) const {
            throw BudgetExceeded(message + " at character " + std::to_string(cursor_));
        }

        void skip_space() {
            while (cursor_ < input_.size() &&
                std::isspace(static_cast<unsigned char>(input_[cursor_])) != 0) {
                ++cursor_;
            }
        }

        bool take(char expected) {
            skip_space();
            if (cursor_ < input_.size() && input_[cursor_] == expected) {
                ++cursor_;
                return true;
            }
            return false;
        }

        NbtValue read_value(bool root) {
            skip_space();
            if (cursor_ >= input_.size()) {
                fail("expected a value");
            }
            budget_.spend(kElementCost);
            const char c = input_[cursor_];
            if (c == '{') {
                return read_map();
            }
            if (c == '[') {
                return read_list_or_array();
            }
            if (c == '"' || c == '\'') {
                return NbtValue(read_quoted());
            }
            if (root) {
                fail("a command value has to be a map");
            }
            return read_unquoted();
        }

        NbtValue read_map() {
            const Nest nest(*this);
            take('{');
            NbtCompound children;
            if (take('}')) {
                return NbtValue(std::move(children));
            }
            while (true) {
                const std::string key = read_key();
                if (!take(':')) {
                    fail("expected ':' after a name");
                }
                children.insert_or_assign(key, read_value(false));
                if (take(',')) {
                    continue;
                }
                if (take('}')) {
                    return NbtValue(std::move(children));
                }
                fail("expected ',' or '}'");
            }
        }

        std::string read_key() {
            skip_space();
            if (cursor_ < input_.size() && (input_[cursor_] == '"' || input_[cursor_] == '\'')) {
                return read_quoted();
            }
            const auto start = cursor_;
            while (cursor_ < input_.size()) {
                const char c = input_[cursor_];
                if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_' && c != '.' &&
                    c != '+' && c != '-') {
                    break;
                }
                ++cursor_;
            }
            if (cursor_ == start) {
                fail("expected a name");
            }
            return std::string(input_.substr(start, cursor_ - start));
        }

        NbtValue read_list_or_array() {
            const Nest nest(*this);
            take('[');

            // A letter and a semicolon right after the bracket mark a typed array; anything
            // else is a plain list of values.
            if (cursor_ + 1 < input_.size() && input_[cursor_ + 1] == ';') {
                const char tag_letter = input_[cursor_];
                if (tag_letter == 'B' || tag_letter == 'I' || tag_letter == 'L') {
                    cursor_ += 2;
                    return read_array(tag_letter);
                }
            }

            NbtList items;
            if (take(']')) {
                return NbtValue(std::move(items));
            }
            while (true) {
                auto element = read_value(false);
                // A list carries one element type, so a mixed one is a typo, not a value.
                if (!items.empty() && items.front().tag() != element.tag()) {
                    fail("a list cannot mix element types");
                }
                items.push_back(std::move(element));
                if (take(',')) {
                    continue;
                }
                if (take(']')) {
                    return NbtValue(std::move(items));
                }
                fail("expected ',' or ']'");
            }
        }

        NbtValue read_array(char kind) {
            NbtBytes bytes;
            NbtInts ints;
            NbtLongs longs;
            while (!take(']')) {
                auto element = read_unquoted();
                const auto number = element.as_long();
                if (!number.has_value()) {
                    fail("arrays hold numbers");
                }
                if (kind == 'B') {
                    // A byte array holds signed bytes; the writer prints them the same way,
                    // so a value survives the round trip instead of changing meaning.
                    if (*number < -128 || *number > 127) {
                        fail("byte out of range");
                    }
                    bytes.push_back(static_cast<std::uint8_t>(*number & 0xFF));
                }
                else if (kind == 'I') {
                    if (*number < INT32_MIN || *number > INT32_MAX) {
                        fail("int out of range");
                    }
                    ints.push_back(static_cast<std::int32_t>(*number));
                }
                else {
                    longs.push_back(*number);
                }
                if (take(']')) {
                    break;
                }
                if (!take(',')) {
                    fail("expected ',' inside an array");
                }
            }
            if (kind == 'B') return NbtValue(std::move(bytes));
            if (kind == 'I') return NbtValue(std::move(ints));
            return NbtValue(std::move(longs));
        }

        std::string read_quoted() {
            const char quote = input_[cursor_++];
            std::string out;
            while (cursor_ < input_.size()) {
                const char c = input_[cursor_++];
                if (c == quote) {
                    return out;
                }
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }
                if (cursor_ >= input_.size()) {
                    fail("unfinished escape");
                }
                const char escaped = input_[cursor_++];
                switch (escaped) {
                case 't': out.push_back('\t'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 's': out.push_back(' '); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case '\'': out.push_back('\''); break;
                case 'x': out.push_back(static_cast<char>(read_hex(2))); break;
                case 'u': read_unicode_escape(out); break;
                default: out.push_back(escaped); break;
                }
            }
            fail("unterminated text");
        }

        static void append_code_point(std::string& out, std::uint32_t code_point) {
            codec::append_utf8(out, code_point);
        }

        // "\uD83D\uDE00" is one character in two escapes: a high surrogate only means
        // something together with the low surrogate behind it, and encoding each half on its
        // own would write two invalid UTF-8 sequences instead of one emoji.
        void read_unicode_escape(std::string& out) {
            std::uint32_t code_point = read_hex(4);

            if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                if (cursor_ + 1 >= input_.size() || input_[cursor_] != '\\' ||
                    input_[cursor_ + 1] != 'u') {
                    fail("a high surrogate has to be followed by a low surrogate");
                }
                cursor_ += 2;
                const std::uint32_t low = read_hex(4);
                if (low < 0xDC00 || low > 0xDFFF) {
                    fail("not a low surrogate");
                }
                code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
            }
            else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                fail("a low surrogate cannot start a character");
            }

            append_code_point(out, code_point);
        }

        std::uint32_t read_hex(int digits) {
            std::uint32_t value = 0;
            for (int i = 0; i < digits; ++i) {
                if (cursor_ >= input_.size()) {
                    fail("truncated escape");
                }
                const char c = input_[cursor_++];
                int digit = -1;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                if (digit < 0) {
                    fail("not a hex digit");
                }
                value = value * 16 + static_cast<std::uint32_t>(digit);
            }
            return value;
        }

        // A bare word is a boolean, a number with its width in the suffix, or text.
        NbtValue read_unquoted() {
            skip_space();
            if (cursor_ < input_.size() && (input_[cursor_] == '"' || input_[cursor_] == '\'')) {
                return NbtValue(read_quoted());
            }

            const auto start = cursor_;
            while (cursor_ < input_.size()) {
                const char c = input_[cursor_];
                if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == ',' || c == ']' ||
                    c == '}' || c == ';') {
                    break;
                }
                ++cursor_;
            }
            const std::string word(input_.substr(start, cursor_ - start));
            if (word.empty()) {
                fail("expected a value");
            }
            if (word == "true") {
                return NbtValue(static_cast<std::int8_t>(1));
            }
            if (word == "false") {
                return NbtValue(static_cast<std::int8_t>(0));
            }
            if (auto number = as_number(word)) {
                return std::move(*number);
            }
            return NbtValue(word);
        }

        std::optional<NbtValue> as_number(const std::string& word) {
            if (word.empty()) {
                return std::nullopt;
            }

            char suffix = 0;
            std::string_view digits = word;
            const char last = static_cast<char>(std::tolower(
                static_cast<unsigned char>(word.back())));
            if (last == 'b' || last == 's' || last == 'l' || last == 'f' || last == 'd') {
                suffix = last;
                digits = std::string_view(word).substr(0, word.size() - 1);
            }
            if (digits.empty()) {
                return std::nullopt;
            }

            if (auto special = as_special_real(digits, suffix)) {
                return special;
            }

            const bool integer = is_integer(digits);
            const bool real = is_real(digits);
            if (!integer && !real) {
                return std::nullopt;
            }

            const std::string text(digits);
            switch (suffix) {
            case 'b': {
                if (!integer) return std::nullopt;
                const auto value = parse_integer(text);
                if (value < -128 || value > 127) {
                    fail("byte out of range");
                }
                return NbtValue(static_cast<std::int8_t>(value));
            }
            case 's': {
                if (!integer) return std::nullopt;
                const auto value = parse_integer(text);
                if (value < -32768 || value > 32767) {
                    fail("short out of range");
                }
                return NbtValue(static_cast<std::int16_t>(value));
            }
            case 'l': {
                if (!integer) return std::nullopt;
                return NbtValue(static_cast<std::int64_t>(parse_integer(text)));
            }
            case 'f': {
                if (integer) {
                    return NbtValue(static_cast<float>(parse_integer(text)));
                }
                return NbtValue(static_cast<float>(parse_real(text)));
            }
            case 'd': {
                if (integer) {
                    return NbtValue(static_cast<double>(parse_integer(text)));
                }
                return NbtValue(parse_real(text));
            }
            default:
                break;
            }

            if (integer) {
                const auto value = parse_integer(text);
                if (value >= INT32_MIN && value <= INT32_MAX) {
                    return NbtValue(static_cast<std::int32_t>(value));
                }
                return NbtValue(static_cast<std::int64_t>(value));
            }
            return NbtValue(parse_real(text));
        }

        // A literal the text cannot hold is malformed input, not a value to clamp to the
        // largest one that fits: strtoll/strtod report it through ERANGE.
        long long parse_integer(const std::string& text) {
            errno = 0;
            char* end = nullptr;
            const auto value = std::strtoll(text.c_str(), &end, 10);
            if (errno == ERANGE) {
                fail("number does not fit in 64 bits");
            }
            if (end != text.c_str() + text.size()) {
                fail("not a complete number");
            }
            return value;
        }

        double parse_real(const std::string& text) {
            errno = 0;
            char* end = nullptr;
            const auto value = std::strtod(text.c_str(), &end);
            if (errno == ERANGE) {
                fail("number does not fit in a double");
            }
            if (end != text.c_str() + text.size()) {
                fail("not a complete number");
            }
            return value;
        }

        // "NaN", "Infinity" and "-Infinity", the three values the writer prints without
        // digits. Anything else goes through the numeric grammar.
        std::optional<NbtValue> as_special_real(std::string_view digits, char suffix) const {
            bool negative = false;
            if (!digits.empty() && (digits.front() == '-' || digits.front() == '+')) {
                negative = digits.front() == '-';
                digits = digits.substr(1);
            }

            std::string lower(digits);
            for (char& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            double value = 0.0;
            if (lower == "nan") {
                value = std::numeric_limits<double>::quiet_NaN();
            }
            else if (lower == "inf" || lower == "infinity") {
                value = std::numeric_limits<double>::infinity();
            }
            else {
                return std::nullopt;
            }

            if (negative) {
                value = -value;
            }
            if (suffix == 'f') {
                return NbtValue(static_cast<float>(value));
            }
            return NbtValue(value);
        }

        static bool is_integer(std::string_view text) {
            if (text.empty()) {
                return false;
            }
            std::size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
            if (i >= text.size()) {
                return false;
            }
            for (; i < text.size(); ++i) {
                if (std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
                    return false;
                }
            }
            return true;
        }

        static bool is_real(std::string_view text) {
            if (text.empty()) {
                return false;
            }
            bool digits = false;
            bool dot = false;
            bool exponent = false;
            std::size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
            for (; i < text.size(); ++i) {
                const char c = text[i];
                if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
                    digits = true;
                    continue;
                }
                if (c == '.' && !dot && !exponent) {
                    dot = true;
                    continue;
                }
                if ((c == 'e' || c == 'E') && digits && !exponent) {
                    exponent = true;
                    continue;
                }
                if ((c == '-' || c == '+') && exponent &&
                    (text[i - 1] == 'e' || text[i - 1] == 'E')) {
                    continue;
                }
                return false;
            }
            return digits;
        }
    };

    // Reads one value from text. The error says what the reader expected, the way a failed
    // command tells the player.
    inline std::optional<NbtValue> parse_snbt(std::string_view input, std::string& error) {
        try {
            SnbtReader reader(input);
            auto value = reader.read_all();
            LOG_TRACE(Data, "tag text parsed: {} characters into a {}", input.size(),
                tag_name(value.tag()));
            return value;
        }
        catch (const std::exception& exception) {
            error = exception.what();
            LOG_DEBUG(Data, "tag text rejected: {}", error);
            return std::nullopt;
        }
    }

    inline std::optional<NbtValue> parse_snbt(std::string_view input) {
        std::string ignored;
        return parse_snbt(input, ignored);
    }

} // namespace terila::nbt
