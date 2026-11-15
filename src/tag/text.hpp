#pragma once

// The text form of a tag tree: how a command writes a value and how the server prints it
// back. Reading and writing live together here because both are about the same grammar —
// a number keeps its width in a suffix, a list keeps one element type, and a name is
// quoted only when it would otherwise read back as something else.

#include "tag/codec.hpp"
#include "tag/value.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terila::tag {

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

        inline std::string real(float value) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
            return buffer;
        }

        inline std::string real(double value) {
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

        inline void write(std::string& out, const TagValue& value, bool indented, int depth) {
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
            else if (const auto* v = value.get_if<TagBytes>()) {
                out += "[B;";
                for (std::size_t i = 0; i < v->size(); ++i) {
                    if (i != 0) out.push_back(',');
                    if (indented) out.push_back(' ');
                    out += std::to_string(static_cast<int>((*v)[i]));
                    out.push_back('B');
                }
                out.push_back(']');
            }
            else if (const auto* v = value.get_if<TagInts>()) {
                out += "[I;";
                for (std::size_t i = 0; i < v->size(); ++i) {
                    if (i != 0) out.push_back(',');
                    if (indented) out.push_back(' ');
                    out += std::to_string((*v)[i]);
                }
                out.push_back(']');
            }
            else if (const auto* v = value.get_if<TagLongs>()) {
                out += "[L;";
                for (std::size_t i = 0; i < v->size(); ++i) {
                    if (i != 0) out.push_back(',');
                    if (indented) out.push_back(' ');
                    out += std::to_string((*v)[i]);
                    out.push_back('L');
                }
                out.push_back(']');
            }
            else if (const auto* v = value.get_if<TagList>()) {
                out.push_back('[');
                for (std::size_t i = 0; i < v->size(); ++i) {
                    if (i != 0) out.push_back(',');
                    break_line(out, indented, depth + 1);
                    write(out, (*v)[i], indented, depth + 1);
                }
                if (!v->empty()) break_line(out, indented, depth);
                out.push_back(']');
            }
            else if (const auto* v = value.get_if<TagMap>()) {
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
    inline std::string to_text(const TagValue& value) {
        std::string out;
        text::write(out, value, false, 0);
        return out;
    }

    // The indented form the game's data dump uses.
    inline std::string to_pretty(const TagValue& value) {
        std::string out;
        text::write(out, value, true, 0);
        return out;
    }

    // One line per entry with the type of its value: the shape of a tree, for logs.
    inline std::vector<std::string> describe_tree(const TagValue& value) {
        std::vector<std::string> lines;
        std::function<void(const TagValue&, int)> walk = [&](const TagValue& node, int depth) {
            const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
            const auto descend = [&](const TagValue& child) {
                if (child.get_if<TagMap>() != nullptr || child.get_if<TagList>() != nullptr) {
                    walk(child, depth + 1);
                }
            };
            if (const auto* map = node.get_if<TagMap>()) {
                for (const auto& [key, child] : *map) {
                    lines.push_back(indent + key + ": " + std::string(tag_label(child.tag())));
                    descend(child);
                }
                return;
            }
            if (const auto* list = node.get_if<TagList>()) {
                for (const auto& child : *list) {
                    lines.push_back(indent + "- " + std::string(tag_label(child.tag())));
                    descend(child);
                }
            }
        };
        walk(value, 0);
        return lines;
    }

    // A chat component is a map with a "text" entry, written as a standalone value.
    inline std::vector<std::uint8_t> text_component_bytes(std::string_view message) {
        TagMap component;
        component.insert_or_assign("text", TagValue(std::string(message)));
        return encode_value(TagValue(std::move(component)));
    }

    // ---- reading text ------------------------------------------------------------

    class TagTextReader {
    public:
        explicit TagTextReader(std::string_view input) : input_(input) {}

        // Reads one value and insists that nothing is left over.
        TagValue read_all() {
            auto value = read_value(false);
            skip_space();
            if (cursor_ < input_.size()) {
                throw BudgetExceeded("trailing data after the tag");
            }
            return value;
        }

    private:
        std::string_view input_;
        std::size_t cursor_ = 0;

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

        TagValue read_value(bool root) {
            skip_space();
            if (cursor_ >= input_.size()) {
                fail("expected a value");
            }
            const char c = input_[cursor_];
            if (c == '{') {
                return read_map();
            }
            if (c == '[') {
                return read_list_or_array();
            }
            if (c == '"' || c == '\'') {
                return TagValue(read_quoted());
            }
            if (root) {
                fail("a command value has to be a map");
            }
            return read_unquoted();
        }

        TagValue read_map() {
            take('{');
            TagMap children;
            if (take('}')) {
                return TagValue(std::move(children));
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
                    return TagValue(std::move(children));
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

        TagValue read_list_or_array() {
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

            TagList items;
            if (take(']')) {
                return TagValue(std::move(items));
            }
            while (true) {
                items.push_back(read_value(false));
                if (take(',')) {
                    continue;
                }
                if (take(']')) {
                    return TagValue(std::move(items));
                }
                fail("expected ',' or ']'");
            }
        }

        TagValue read_array(char kind) {
            TagBytes bytes;
            TagInts ints;
            TagLongs longs;
            while (!take(']')) {
                auto element = read_unquoted();
                const auto number = element.as_long();
                if (!number.has_value()) {
                    fail("arrays hold numbers");
                }
                if (kind == 'B') {
                    if (*number < -128 || *number > 255) {
                        fail("byte out of range");
                    }
                    bytes.push_back(static_cast<std::uint8_t>(*number & 0xFF));
                }
                else if (kind == 'I') {
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
            if (kind == 'B') return TagValue(std::move(bytes));
            if (kind == 'I') return TagValue(std::move(ints));
            return TagValue(std::move(longs));
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
                case 'u': append_code_point(out, read_hex(4)); break;
                default: out.push_back(escaped); break;
                }
            }
            fail("unterminated text");
        }

        static void append_code_point(std::string& out, std::uint32_t code_point) {
            codec::append_utf8(out, code_point);
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
        TagValue read_unquoted() {
            skip_space();
            if (cursor_ < input_.size() && (input_[cursor_] == '"' || input_[cursor_] == '\'')) {
                return TagValue(read_quoted());
            }

            const auto start = cursor_;
            while (cursor_ < input_.size()) {
                const char c = input_[cursor_];
                if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == ',' || c == ']' ||
                    c == '}' || c == ':' || c == ';') {
                    break;
                }
                ++cursor_;
            }
            const std::string word(input_.substr(start, cursor_ - start));
            if (word.empty()) {
                fail("expected a value");
            }
            if (word == "true") {
                return TagValue(static_cast<std::int8_t>(1));
            }
            if (word == "false") {
                return TagValue(static_cast<std::int8_t>(0));
            }
            if (auto number = as_number(word)) {
                return std::move(*number);
            }
            return TagValue(word);
        }

        static std::optional<TagValue> as_number(const std::string& word) {
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

            const bool integer = is_integer(digits);
            const bool real = is_real(digits);
            if (!integer && !real) {
                return std::nullopt;
            }

            const std::string text(digits);
            switch (suffix) {
            case 'b': {
                if (!integer) return std::nullopt;
                return TagValue(static_cast<std::int8_t>(
                    std::strtoll(text.c_str(), nullptr, 10) & 0xFF));
            }
            case 's': {
                if (!integer) return std::nullopt;
                return TagValue(static_cast<std::int16_t>(
                    std::strtoll(text.c_str(), nullptr, 10)));
            }
            case 'l': {
                if (!integer) return std::nullopt;
                return TagValue(static_cast<std::int64_t>(std::strtoll(text.c_str(), nullptr, 10)));
            }
            case 'f': {
                return TagValue(std::strtof(text.c_str(), nullptr));
            }
            case 'd': {
                return TagValue(std::strtod(text.c_str(), nullptr));
            }
            default:
                break;
            }

            if (integer) {
                const auto value = std::strtoll(text.c_str(), nullptr, 10);
                if (value >= INT32_MIN && value <= INT32_MAX) {
                    return TagValue(static_cast<std::int32_t>(value));
                }
                return TagValue(static_cast<std::int64_t>(value));
            }
            return TagValue(std::strtod(text.c_str(), nullptr));
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
    inline std::optional<TagValue> parse_tag(std::string_view input, std::string& error) {
        try {
            TagTextReader reader(input);
            auto value = reader.read_all();
            LOG_TRACE(Data, "tag text parsed: {} characters into a {}", input.size(),
                tag_label(value.tag()));
            return value;
        }
        catch (const std::exception& exception) {
            error = exception.what();
            LOG_DEBUG(Data, "tag text rejected: {}", error);
            return std::nullopt;
        }
    }

    inline std::optional<TagValue> parse_tag(std::string_view input) {
        std::string ignored;
        return parse_tag(input, ignored);
    }

} // namespace terila::tag
