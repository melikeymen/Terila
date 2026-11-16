#pragma once

// Where the server reads its data files from.
//
// A build either carries a copy of the files it needs or reads them from input/ at run time.
// TERILA_EMBED_INPUT on the CMake command line turns on the first case and reaches the code
// as TERILA_EMBEDDED_INPUT; the table generated at build time then answers every read from
// memory and the server starts without an input/ directory at all.
//
// Call sites only speak relative paths such as "packets/ids.json", so both modes stay
// interchangeable and nothing outside this file knows which one is in use.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace terila::data::input {

    // Points the disk reader at a directory. Embedded builds ignore the call: their data is
    // already in the binary.
    void mount(std::string root);
    const std::string& root();

    bool is_embedded() noexcept;

    bool read(std::string_view relative, std::vector<std::uint8_t>& out);
    bool read_text(std::string_view relative, std::string& out);

    // Every file below a directory, relative to the input root and sorted, so walking the
    // registry payloads does not depend on directory order or on a filesystem being present.
    std::vector<std::string> files_under(std::string_view prefix);

    // What the build carries, for the startup log. Disk builds report zero bytes: counting
    // would mean walking the tree for a line nobody needs.
    std::size_t file_count() noexcept;
    std::size_t total_bytes() noexcept;

    // "embedded, 172 files, 19.0 MB" or "disk, C:/.../input"
    std::string describe();

} // namespace terila::data::input
