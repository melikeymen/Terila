#pragma once

#include "commands/command_context.hpp"

#include <string_view>

namespace terila::commands {

    // Runs one command line, without the leading slash. False when no command has that
    // name, which is what the caller turns into the command list.
    bool dispatch(const Context& context, std::string_view line);

} // namespace terila::commands
