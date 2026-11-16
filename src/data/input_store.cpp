#include "data/input_store.hpp"

#include "log/logger.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#ifdef TERILA_EMBEDDED_INPUT
// Written by tools/embed_input.cpp into the build directory: the files compiled into the
// binary, as a sorted table of path -> chunks of bytes.
#include "embedded_input.hpp"
#endif

namespace terila::data::input {

    namespace {

        std::string g_root;

#ifdef TERILA_EMBEDDED_INPUT

        // The table is sorted by CMake (the manifest lists files in sorted order), so a lookup
        // is a binary search over string comparisons instead of a walk.
        const terila::embedded::Entry* find_entry(std::string_view relative) noexcept {
            const auto begin = terila::embedded::entries;
            const auto end = begin + terila::embedded::entry_count;

            const auto it = std::lower_bound(begin, end, relative,
                [](const terila::embedded::Entry& entry, std::string_view path) {
                    return entry.path < path;
                });
            if (it == end || it->path != relative) {
                return nullptr;
            }
            return it;
        }

#endif

        bool read_disk(std::string_view relative, std::vector<std::uint8_t>& out) {
            const std::filesystem::path path = std::filesystem::path(g_root) / std::string(relative);

            std::ifstream file(path, std::ios::binary);
            if (!file) {
                LOG_WARN(Data, "cannot open input file: {}", path.string());
                return false;
            }

            file.seekg(0, std::ios::end);
            const auto size = file.tellg();
            file.seekg(0, std::ios::beg);
            if (size < 0) {
                LOG_WARN(Data, "cannot size input file: {}", path.string());
                return false;
            }

            out.resize(static_cast<std::size_t>(size));
            if (!out.empty()) {
                file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
            }
            if (!file && !out.empty()) {
                LOG_WARN(Data, "short read on input file: {}", path.string());
                return false;
            }
            return true;
        }

        std::vector<std::string> files_under_disk(std::string_view prefix) {
            std::vector<std::string> files;

            const std::filesystem::path base(g_root);
            std::error_code ec;
            if (!std::filesystem::exists(base, ec) || ec) {
                return files;
            }

            for (const auto& entry : std::filesystem::recursive_directory_iterator(base, ec)) {
                if (ec) {
                    break;
                }
                std::error_code type_ec;
                if (!entry.is_regular_file(type_ec) || type_ec) {
                    continue;
                }
                const auto relative = entry.path().lexically_relative(base).generic_string();
                if (relative.rfind(prefix, 0) == 0) {
                    files.push_back(relative);
                }
            }

            std::sort(files.begin(), files.end());
            return files;
        }

    } // namespace

    void mount(std::string root) {
        g_root = std::move(root);
    }

    const std::string& root() {
        return g_root;
    }

    bool is_embedded() noexcept {
#ifdef TERILA_EMBEDDED_INPUT
        return true;
#else
        return false;
#endif
    }

    bool read(std::string_view relative, std::vector<std::uint8_t>& out) {
#ifdef TERILA_EMBEDDED_INPUT
        const auto* entry = find_entry(relative);
        if (entry == nullptr) {
            LOG_WARN(Data, "not compiled in: {} (add it to TERILA_EMBED_FILES)", relative);
            return false;
        }

        out.clear();
        std::size_t total = 0;
        for (std::size_t i = 0; i < entry->chunk_count; ++i) {
            total += entry->chunks[i].size;
        }
        out.reserve(total);

        for (std::size_t i = 0; i < entry->chunk_count; ++i) {
            const auto& chunk = entry->chunks[i];
            out.insert(out.end(), chunk.data, chunk.data + chunk.size);
        }
        return true;
#else
        return read_disk(relative, out);
#endif
    }

    bool read_text(std::string_view relative, std::string& out) {
        std::vector<std::uint8_t> bytes;
        if (!read(relative, bytes)) {
            return false;
        }
        out.assign(bytes.begin(), bytes.end());
        return true;
    }

    std::vector<std::string> files_under(std::string_view prefix) {
#ifdef TERILA_EMBEDDED_INPUT
        std::vector<std::string> files;
        for (std::size_t i = 0; i < terila::embedded::entry_count; ++i) {
            const auto path = terila::embedded::entries[i].path;
            if (path.rfind(prefix, 0) == 0) {
                files.emplace_back(path);
            }
        }
        return files;
#else
        return files_under_disk(prefix);
#endif
    }

    std::size_t file_count() noexcept {
#ifdef TERILA_EMBEDDED_INPUT
        return terila::embedded::entry_count;
#else
        return 0;
#endif
    }

    std::size_t total_bytes() noexcept {
#ifdef TERILA_EMBEDDED_INPUT
        std::size_t total = 0;
        for (std::size_t i = 0; i < terila::embedded::entry_count; ++i) {
            for (std::size_t c = 0; c < terila::embedded::entries[i].chunk_count; ++c) {
                total += terila::embedded::entries[i].chunks[c].size;
            }
        }
        return total;
#else
        return 0;
#endif
    }

    std::string describe() {
#ifdef TERILA_EMBEDDED_INPUT
        const auto bytes = total_bytes();
        char buffer[64] = {};
        if (bytes >= 1024 * 1024) {
            std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
        else {
            std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
        }
        return "embedded, " + std::to_string(file_count()) + " files, " + buffer;
#else
        return "disk, " + (g_root.empty() ? std::string("(not mounted)") : g_root);
#endif
    }

} // namespace terila::data::input
