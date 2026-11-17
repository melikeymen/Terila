#pragma once

// The argument grammar of commands that take another command:
//   /execute [as <targets>] run <command>
//
// The executed command is kept as the rest of the line, so it is not split any further
// here: whatever follows `run` is handed to the command dispatcher as one string.

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace terila::network::commands {

    namespace detail {

        inline std::string_view trim(std::string_view text) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
                text.remove_prefix(1);
            }
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
                text.remove_suffix(1);
            }
            return text;
        }

        // The next whitespace-delimited word, with `rest` left after it.
        inline std::string_view token(std::string_view text, std::string_view& rest) {
            text = trim(text);
            std::size_t end = 0;
            while (end < text.size() &&
                std::isspace(static_cast<unsigned char>(text[end])) == 0) {
                ++end;
            }
            rest = text.substr(end);
            return text.substr(0, end);
        }

        inline std::string lower(std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

    } // namespace detail

    struct Execute {
        // Empty when the command names no target: it then runs as the caller.
        std::string selector;
        std::string command;
    };

    // Returns nothing when the line does not fit the grammar. A missing `run` is an error
    // rather than an empty command, so `/execute as @a` cannot silently run nothing.
    inline std::optional<Execute> parse_execute(std::string_view arguments) {
        std::string_view rest = detail::trim(arguments);
        if (rest.empty()) {
            return std::nullopt;
        }

        const std::string keyword = detail::lower(detail::token(rest, rest));
        if (keyword == "run") {
            const std::string_view nested = detail::trim(rest);
            if (nested.empty()) {
                return std::nullopt;
            }
            return Execute{ {}, std::string(nested) };
        }
        if (keyword != "as") {
            return std::nullopt;
        }

        const std::string_view targets = detail::token(rest, rest);
        if (targets.empty()) {
            return std::nullopt;
        }
        const std::string_view run_word = detail::token(rest, rest);
        if (!run_word.empty() && detail::lower(run_word) != "run") {
            return std::nullopt;
        }
        const std::string_view nested = detail::trim(rest);
        if (nested.empty()) {
            return std::nullopt;
        }
        return Execute{ detail::lower(targets), std::string(nested) };
    }

} // namespace terila::network::commands
