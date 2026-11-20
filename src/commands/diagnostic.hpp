#pragma once

#include <cstddef>
#include <string>

namespace terila::commands {

    // How a rejected command line failed. The distinction matters to a caller that wants
    // to report one line differently, and to /execute if, which has to tell an unparsable
    // tail from a tail that ran and failed.
    enum class Problem {
        Unknown,     // no such command or argument
        Incomplete,  // a command is typed but not finished
        Invalid,     // a value does not fit the argument
        Permission,  // the source may not run this node
        Failed       // the command ran and refused to do the work
    };

    // A rejected line: where, why, and what was expected there. The message never carries
    // its own caret; the caller places `offset` in front of it.
    struct Diagnostic {
        Problem kind = Problem::Unknown;
        std::size_t offset = 0;
        std::string message;

        explicit operator bool() const noexcept { return !message.empty(); }
    };

} // namespace terila::commands
