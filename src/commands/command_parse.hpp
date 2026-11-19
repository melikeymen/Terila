#pragma once

// The argument grammar of commands that take another command:
//   /execute [as <targets>] run <command>
//
// The executed command is kept as the rest of the line, so it is not split any further
// here: whatever follows `run` is handed to the dispatcher as one string.

#include "commands/text.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace terila::commands {

    struct Execute {
        // Empty when the command names no target: it then runs as the caller.
        std::string selector;
        std::string command;
    };

    // Returns nothing when the line does not fit the grammar. A missing `run` is an error
    // rather than an empty command, so `/execute as @a` cannot silently run nothing.
    inline std::optional<Execute> parse_execute(std::string_view arguments) {
        std::string_view rest = trim(arguments);
        if (rest.empty()) {
            return std::nullopt;
        }

        const std::string keyword = lower(token(rest, rest));
        if (keyword == "run") {
            const std::string_view nested = trim(rest);
            if (nested.empty()) {
                return std::nullopt;
            }
            return Execute{ {}, std::string(nested) };
        }
        if (keyword != "as") {
            return std::nullopt;
        }

        const std::string_view targets = token(rest, rest);
        if (targets.empty()) {
            return std::nullopt;
        }
        const std::string_view run_word = token(rest, rest);
        if (!run_word.empty() && lower(run_word) != "run") {
            return std::nullopt;
        }
        const std::string_view nested = trim(rest);
        if (nested.empty()) {
            return std::nullopt;
        }
        return Execute{ lower(targets), std::string(nested) };
    }

} // namespace terila::commands
