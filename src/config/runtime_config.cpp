#include "config/runtime_config.hpp"

#include "log/logger.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace terila::config {

    namespace {

        std::string trim(std::string_view text) {
            auto start = text.begin();
            while (start != text.end() && std::isspace(static_cast<unsigned char>(*start))) {
                ++start;
            }
            auto end = text.end();
            while (end != start && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
                --end;
            }
            return std::string(start, end);
        }

        std::string lowered(std::string text) {
            for (char& c : text) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return text;
        }

        bool parse_number(const std::string& value, std::size_t& out) {
            try {
                std::size_t consumed = 0;
                const auto parsed = std::stoll(value, &consumed);
                if (consumed != value.size() || parsed < 0) {
                    return false;
                }
                out = static_cast<std::size_t>(parsed);
                return true;
            }
            catch (...) {
                return false;
            }
        }

    } // namespace

    std::string default_runtime_config_text(const RuntimeConfig& config) {
        std::ostringstream out;
        out <<
            "# Terila runtime configuration.\n"
            "#\n"
            "# Written on the first start and read back on every start afterwards; delete this\n"
            "# file to have the defaults written again. server.properties keeps the\n"
            "# Minecraft-facing settings (port, view distance, gamemode), this file decides how\n"
            "# much of the machine the process may use.\n"
            "#\n"
            "# Values are plain integers. A missing or unreadable value falls back to the\n"
            "# default instead of stopping the server.\n"
            "\n"
            "[memory]\n"
            "# Ceiling for the memory the server keeps around, in MiB. The chunk caches are\n"
            "# sized from it, and the working set is capped to it where the platform allows.\n"
            "# 0 disables the ceiling.\n"
            "max-megabytes=" << config.max_memory_mb << "\n"
            "\n"
            "# Memory kept free for the rest of the machine, in MiB. The caches leave this\n"
            "# much of the ceiling unused. 0 disables the reserve.\n"
            "min-megabytes=" << config.min_memory_mb << "\n"
            "\n"
            "[threads]\n"
            "# Upper bound for the chunk generation workers. One core is left to the io\n"
            "# thread, so the pool usually starts with one less than the machine has.\n"
            "max=" << config.max_threads << "\n"
            "\n"
            "# Lower bound: the pool never starts fewer than this, even on a small machine.\n"
            "min=" << config.min_threads << "\n";
        return out.str();
    }

    bool ensure_runtime_config(const std::filesystem::path& directory,
        const RuntimeConfig& defaults, std::filesystem::path* written) {
        if (written != nullptr) {
            written->clear();
        }

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (!std::filesystem::is_directory(directory, ec)) {
            LOG_WARN(Config, "cannot create the configs directory '{}': {}",
                directory.string(), ec.message());
            return false;
        }

        const auto path = directory / kRuntimeConfigFile;
        if (std::filesystem::exists(path, ec) && !ec) {
            return true;
        }

        std::ofstream out(path, std::ios::binary);
        if (!out) {
            LOG_WARN(Config, "cannot write '{}'", path.string());
            return false;
        }
        out << default_runtime_config_text(defaults);
        if (!out.good()) {
            LOG_WARN(Config, "cannot write '{}'", path.string());
            return false;
        }
        if (written != nullptr) {
            *written = path;
        }
        return true;
    }

    bool load_runtime_config(RuntimeConfig& out, const std::filesystem::path& path) {
        std::ifstream in(path);
        if (!in) {
            return false;
        }

        std::string line;
        std::string section;
        bool saw_value = false;

        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }

            // [memory] and [threads] group the keys; a key that already carries its
            // section (memory.max-megabytes) works without one.
            if (line.front() == '[' && line.back() == ']') {
                section = lowered(trim(std::string_view(line).substr(1, line.size() - 2)));
                continue;
            }

            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }

            auto key = lowered(trim(std::string_view(line).substr(0, eq)));
            const auto value = trim(std::string_view(line).substr(eq + 1));
            if (key.find('.') == std::string::npos && !section.empty()) {
                key = section + "." + key;
            }

            std::size_t number = 0;
            if (key == "memory.max-megabytes" || key == "memory.max" ||
                key == "max-megabytes") {
                if (parse_number(value, number)) {
                    out.max_memory_mb = number;
                    saw_value = true;
                }
                else {
                    LOG_WARN(Config, "invalid memory ceiling '{}', keeping {} MiB", value,
                        out.max_memory_mb);
                }
            }
            else if (key == "memory.min-megabytes" || key == "memory.min" ||
                key == "min-megabytes") {
                if (parse_number(value, number)) {
                    out.min_memory_mb = number;
                    saw_value = true;
                }
                else {
                    LOG_WARN(Config, "invalid memory reserve '{}', keeping {} MiB", value,
                        out.min_memory_mb);
                }
            }
            else if (key == "threads.max" || key == "threads.max-threads" ||
                key == "max-threads") {
                if (parse_number(value, number)) {
                    out.max_threads = static_cast<unsigned>(
                        std::min<std::size_t>(number, detail::kMaxThreads));
                    saw_value = true;
                }
                else {
                    LOG_WARN(Config, "invalid thread ceiling '{}', keeping {}", value,
                        out.max_threads);
                }
            }
            else if (key == "threads.min" || key == "threads.min-threads" ||
                key == "min-threads") {
                if (parse_number(value, number)) {
                    out.min_threads = static_cast<unsigned>(
                        std::min<std::size_t>(number, detail::kMaxThreads));
                    saw_value = true;
                }
                else {
                    LOG_WARN(Config, "invalid thread floor '{}', keeping {}", value,
                        out.min_threads);
                }
            }
        }

        detail::sanitize(out);
        return saw_value;
    }

    void apply_memory_limits(const RuntimeConfig& config) {
#ifdef _WIN32
        if (config.max_memory_mb == 0) {
            LOG_DEBUG(Config, "memory ceiling disabled, the caches use their sizing default");
            return;
        }

        const auto to_bytes = [](std::size_t megabytes) {
            return static_cast<SIZE_T>(megabytes) * 1024 * 1024;
        };
        // A zero minimum asks for the default, so the smaller limit is one MiB instead.
        const SIZE_T min_bytes = to_bytes(config.min_memory_mb == 0 ? 1 : config.min_memory_mb);
        const SIZE_T max_bytes = to_bytes(config.max_memory_mb);
        if (!SetProcessWorkingSetSize(GetCurrentProcess(), min_bytes, max_bytes)) {
            LOG_WARN(Config, "cannot cap the working set at {} MiB (error {}); the ceiling "
                "still sizes the chunk caches", config.max_memory_mb,
                static_cast<unsigned long>(GetLastError()));
            return;
        }
        LOG_INFO(Config, "working set capped between {} and {} MiB", min_bytes / (1024 * 1024),
            max_bytes / (1024 * 1024));
#else
        LOG_DEBUG(Config, "memory ceiling sizes the chunk caches; this platform has no "
            "working-set cap");
        (void)config;
#endif
    }

} // namespace terila::config
