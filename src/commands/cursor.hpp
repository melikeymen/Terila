#pragma once

#include "commands/diagnostic.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace terila::commands {

    // Reads a command line from left to right and remembers where the last problem was.
    // Parsers never index into the line themselves: they hand the cursor to the reader
    // shape they need, and a failure lands on the cursor so the parse can try another
    // branch from the same place.
    class Cursor {
    public:
        explicit Cursor(std::string_view text) : text_(text) {}

        std::string_view text() const noexcept { return text_; }
        std::size_t position() const noexcept { return pos_; }
        void seek(std::size_t position) { pos_ = position; }

        void skip_space() {
            while (pos_ < text_.size() &&
                std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }

        bool at_end() {
            skip_space();
            return pos_ >= text_.size();
        }

        // The next word without consuming it, which is how a branch decides whether it
        // starts with a literal keyword.
        std::string_view peek() {
            skip_space();
            std::size_t end = pos_;
            while (end < text_.size() &&
                std::isspace(static_cast<unsigned char>(text_[end])) == 0) {
                ++end;
            }
            return text_.substr(pos_, end - pos_);
        }

        // Consumes `word` when the line carries it at this position. The comparison is
        // case-insensitive, the same way the command names are matched.
        bool literal(std::string_view word) {
            std::string_view next = peek();
            if (next.size() != word.size()) {
                return false;
            }
            for (std::size_t i = 0; i < word.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(next[i])) !=
                    std::tolower(static_cast<unsigned char>(word[i]))) {
                    return false;
                }
            }
            pos_ += word.size();
            return true;
        }

        std::optional<std::string> word() {
            std::string_view next = peek();
            if (next.empty()) {
                fail(Problem::Incomplete, "expected a word");
                return std::nullopt;
            }
            pos_ += next.size();
            return std::string(next);
        }

        // A quoted phrase or a bare word, the shape names and item ids arrive in.
        std::optional<std::string> phrase() {
            skip_space();
            if (pos_ < text_.size() && (text_[pos_] == '"' || text_[pos_] == '\'')) {
                const char quote = text_[pos_++];
                std::string out;
                while (pos_ < text_.size() && text_[pos_] != quote) {
                    if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
                        ++pos_;
                    }
                    out.push_back(text_[pos_++]);
                }
                if (pos_ >= text_.size()) {
                    fail(Problem::Invalid, "unterminated quote");
                    return std::nullopt;
                }
                ++pos_;
                return out;
            }
            return word();
        }

        // Everything that is left, trimmed. A greedy argument (a nested command, a chat
        // message) owns the rest of the line.
        std::string_view greedy() {
            skip_space();
            std::string_view rest = text_.substr(pos_);
            while (!rest.empty() &&
                std::isspace(static_cast<unsigned char>(rest.back())) != 0) {
                rest.remove_suffix(1);
            }
            pos_ = text_.size();
            if (rest.empty()) {
                fail(Problem::Incomplete, "expected more input");
            }
            return rest;
        }

        std::optional<std::int64_t> integer() {
            std::string_view next = peek();
            if (next.empty()) {
                fail(Problem::Incomplete, "expected a whole number");
                return std::nullopt;
            }
            std::int64_t value = 0;
            const auto result = std::from_chars(next.data(), next.data() + next.size(), value);
            if (result.ec != std::errc{} || result.ptr != next.data() + next.size()) {
                fail(Problem::Invalid, "expected a whole number");
                return std::nullopt;
            }
            pos_ += next.size();
            return value;
        }

        std::optional<double> real() {
            std::string_view next = peek();
            if (next.empty()) {
                fail(Problem::Incomplete, "expected a number");
                return std::nullopt;
            }
            double value = 0.0;
            const auto result = std::from_chars(next.data(), next.data() + next.size(), value);
            if (result.ec != std::errc{} || result.ptr != next.data() + next.size()) {
                fail(Problem::Invalid, "expected a number");
                return std::nullopt;
            }
            pos_ += next.size();
            return value;
        }

        std::optional<bool> boolean() {
            std::string_view next = peek();
            if (next == "true") {
                pos_ += next.size();
                return true;
            }
            if (next == "false") {
                pos_ += next.size();
                return false;
            }
            fail(next.empty() ? Problem::Incomplete : Problem::Invalid, "expected true or false");
            return std::nullopt;
        }

        void fail(Problem kind, std::string message) {
            if (diagnostic_.message.empty()) {
                diagnostic_ = Diagnostic{ kind, pos_, std::move(message) };
            }
        }

        const Diagnostic& diagnostic() const noexcept { return diagnostic_; }
        void clear_diagnostic() { diagnostic_ = Diagnostic{}; }

    private:
        std::string_view text_;
        std::size_t pos_ = 0;
        Diagnostic diagnostic_;
    };

} // namespace terila::commands
