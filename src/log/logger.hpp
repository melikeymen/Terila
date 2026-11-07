#pragma once

#include <format>
#include <source_location>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "log/level.hpp"

namespace terila::log {

    enum class TimestampFormat : std::uint8_t {
        None,
        Seconds,
        Millis,
        Micros,
        Nanos,
        Date,
        Unix,
    };

    struct Config {
        Level           min_level = Level::Info;
        std::uint8_t    scope_mask = 0xFF;
        TimestampFormat timestamp = TimestampFormat::Millis;
        bool            color_enabled = true;
        std::string     file_path = "";
        Level           exit_on_level = Level::Fatal;
        bool            crash_context = false;
        std::size_t     ring_capacity = 256;
    };

    void init(const Config& cfg);
    void shutdown();

    void set_level(Level lvl);
    void set_scope(Scope sc, bool enabled);
    void set_file(const std::string& path);
    void set_color(bool enabled);

    bool is_enabled(Level lvl, Scope sc) noexcept;

    void write(Level lvl, Scope sc, std::string_view msg);

    void write_unexpected(std::string_view reason) noexcept;

    template<typename... Args>
    void logf(Level lvl, Scope sc,
        std::format_string<Args...> fmt, Args&&... args,
        std::source_location loc = std::source_location::current())
    {
        if (!is_enabled(lvl, sc)) return;
        write(lvl, sc, std::format(fmt, std::forward<Args>(args)...));
        (void)loc;
    }

}

#define LOG_TRACE(sc, ...)    ::terila::log::logf(::terila::log::Level::Trace,    ::terila::log::Scope::sc, __VA_ARGS__)
#define LOG_DEBUG(sc, ...)    ::terila::log::logf(::terila::log::Level::Debug,    ::terila::log::Scope::sc, __VA_ARGS__)
#define LOG_INFO(sc, ...)     ::terila::log::logf(::terila::log::Level::Info,     ::terila::log::Scope::sc, __VA_ARGS__)
#define LOG_WARN(sc, ...)     ::terila::log::logf(::terila::log::Level::Warn,     ::terila::log::Scope::sc, __VA_ARGS__)
#define LOG_ERROR(sc, ...)    ::terila::log::logf(::terila::log::Level::Error,    ::terila::log::Scope::sc, __VA_ARGS__)
#define LOG_CRITICAL(sc, ...) ::terila::log::logf(::terila::log::Level::Critical, ::terila::log::Scope::sc, __VA_ARGS__)
#define LOG_FATAL(sc, ...)    ::terila::log::logf(::terila::log::Level::Fatal,    ::terila::log::Scope::sc, __VA_ARGS__)