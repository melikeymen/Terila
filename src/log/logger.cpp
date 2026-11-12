#include "log/logger.hpp"

#include <chrono>
#include <fstream>
#include <mutex>
#include <cstdio>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace terila::log {

    namespace {

        std::atomic<Level>          g_min_level{ Level::Info };
        std::atomic<std::uint8_t>   g_scope_mask{ 0xFF };
        std::atomic<Level>          g_exit_on_level{ Level::Fatal };
        std::atomic<bool>           g_color{ true };
        std::atomic<TimestampFormat> g_timestamp{ TimestampFormat::Millis };

        std::mutex                  g_mutex;
        std::ofstream               g_file;
        bool                        g_initialized = false;

#ifdef _WIN32
        bool                        g_win_ansi = false;
#endif

        char lower(char c) noexcept {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        void enable_windows_ansi() {
#ifdef _WIN32

            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
            HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
            if (h == INVALID_HANDLE_VALUE) return;
            DWORD mode = 0;
            if (!GetConsoleMode(h, &mode)) return;
            if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                g_win_ansi = true;
            }
#endif
        }

        std::string_view level_color(Level lvl) {
            switch (lvl) {
            case Level::Trace:    return "\033[90m";
            case Level::Debug:    return "\033[36m";
            case Level::Info:     return "\033[32m";
            case Level::Warn:     return "\033[33m";
            case Level::Error:    return "\033[31m";
            case Level::Critical: return "\033[1;35m";
            case Level::Fatal:    return "\033[1;31m"; 
            }
            return "\033[0m";
        }

        std::string format_timestamp(TimestampFormat fmt) {
            using namespace std::chrono;
            if (fmt == TimestampFormat::None) return {};

            auto now = system_clock::now();
            auto epoch = now.time_since_epoch();
            auto epoch_ms = duration_cast<milliseconds>(epoch);
            auto ms_part = epoch_ms % 1000;

            auto tp_sec = time_point_cast<seconds>(now);
            auto tp_ms = time_point_cast<milliseconds>(now);
            auto tp_us = time_point_cast<microseconds>(now);
            auto tp_ns = time_point_cast<nanoseconds>(now);

            switch (fmt) {
            case TimestampFormat::Seconds:
                return std::format("{:%H:%M:%S}", tp_sec);
            case TimestampFormat::Millis:
                return std::format("{:%H:%M:%S}.{:03}", tp_sec, ms_part.count());
            case TimestampFormat::Micros: {
                auto us_part = duration_cast<microseconds>(epoch) % 1000000;
                return std::format("{:%H:%M:%S}.{:06}", tp_sec, us_part.count());
            }
            case TimestampFormat::Nanos: {
                auto ns_part = duration_cast<nanoseconds>(epoch) % 1000000000;
                return std::format("{:%H:%M:%S}.{:09}", tp_sec, ns_part.count());
            }
            case TimestampFormat::Date:
                return std::format("{:%Y-%m-%d %H:%M:%S}.{:03}", tp_sec, ms_part.count());
            case TimestampFormat::Unix:
                return std::format("{}.{:03}", epoch_ms.count() / 1000, epoch_ms.count() % 1000);
            case TimestampFormat::None:
                return {};
            }
            return {};
        }

    } // namespace

    void init(const Config& cfg) {
        std::lock_guard<std::mutex> lock(g_mutex);

        g_min_level.store(cfg.min_level);
        g_scope_mask.store(cfg.scope_mask);
        g_exit_on_level.store(cfg.exit_on_level);
        g_timestamp.store(cfg.timestamp);
        g_color.store(cfg.color_enabled);

        if (cfg.color_enabled) enable_windows_ansi();

        if (g_file.is_open()) {
            g_file.close();
        }

        if (!cfg.file_path.empty()) {
            g_file.open(cfg.file_path, std::ios::out | std::ios::app);
            if (!g_file) {
                std::fprintf(stderr, "[logger] failed to open log file: %s\n", cfg.file_path.c_str());
            }
        }

        g_initialized = true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file.is_open()) {
            g_file.flush();
            g_file.close();
        }
        g_initialized = false;
    }

    void set_level(Level lvl) {
        g_min_level.store(lvl);
    }

    void set_scope(Scope sc, bool enabled) {
        auto mask = g_scope_mask.load();
        auto bit = static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(sc));
        mask = enabled ? (mask | bit) : (mask & ~bit);
        g_scope_mask.store(mask);
    }

    void set_file(const std::string& path) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file.is_open()) g_file.close();
        if (!path.empty()) {
            g_file.open(path, std::ios::out | std::ios::app);
        }
    }

    void set_color(bool enabled) {
        g_color.store(enabled);
        if (enabled) enable_windows_ansi();
    }

    bool is_enabled(Level lvl, Scope sc) noexcept {
        if (lvl < g_min_level.load(std::memory_order_relaxed)) return false;
        auto mask = g_scope_mask.load(std::memory_order_relaxed);
        return (mask & (1u << static_cast<std::uint8_t>(sc))) != 0;
    }

    void write(Level lvl, Scope sc, std::string_view msg) {
        if (!is_enabled(lvl, sc)) return;

        std::string ts = format_timestamp(g_timestamp.load());
        bool color = g_color.load();

        std::string level_part;
        level_part += '[';
        level_part += level_name(lvl);
        level_part += "] ";

        std::string line;
        line.reserve(ts.size() + msg.size() + 64);
        line += '[';  line += ts;                  line += "] ";
        line += level_part;
        line += '[';  line += scope_name(sc);      line += "] ";
        line += msg;
        line += '\n';

        std::string console_line;
        console_line.reserve(line.size() + 16);
        if (color) {
            console_line += '[';  console_line += ts;  console_line += "] ";
            console_line += level_color(lvl);
            console_line += level_part;
            console_line += "\033[0m";
            console_line += '[';  console_line += scope_name(sc);  console_line += "] ";
            console_line += msg;
            console_line += '\n';
        }
        else {
            console_line = line;
        }

        std::unique_lock<std::mutex> lock(g_mutex);

        std::fwrite(console_line.data(), 1, console_line.size(), stdout);

        if (g_file.is_open()) {
            g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
            g_file.flush();
        }

        if (lvl >= g_exit_on_level.load()) {
            if (g_file.is_open()) g_file.flush();
            std::fflush(stdout);
            // atexit handlers may log again, so release the mutex before exiting.
            lock.unlock();
            std::exit(7);
        }
    }

    void write_unexpected(std::string_view reason) noexcept {
        std::fprintf(stderr, "\n[UNEXPECTED] %.*s\n",
            static_cast<int>(reason.size()), reason.data());
        std::fflush(stderr);
        std::abort();
    }

} // namespace terila::log