#pragma once

#ifdef _WIN32
#  include <windows.h>
#endif

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

    inline std::string to_utf8(const std::string& s) {
#ifdef _WIN32
        if (s.empty()) return s;

        int wlen = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (wlen <= 0) return s;

        std::wstring wide(wlen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), wide.data(), wlen);

        int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
        if (ulen <= 0) return s;

        std::string out(ulen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, out.data(), ulen, nullptr, nullptr);
        return out;
#else
        return s;
#endif
    }

    template<typename... Args>
    void logf(Level lvl, Scope sc, std::source_location loc,
        std::format_string<Args...> fmt, Args&&... args)
    {
        if (!is_enabled(lvl, sc)) return;
        write(lvl, sc, std::format(fmt, std::forward<Args>(args)...));
        (void)loc;
    }

}

#define LOG_TRACE(sc, ...)    ::terila::log::logf(::terila::log::Level::Trace,    ::terila::log::Scope::sc, std::source_location::current(), __VA_ARGS__)
#define LOG_DEBUG(sc, ...)    ::terila::log::logf(::terila::log::Level::Debug,    ::terila::log::Scope::sc, std::source_location::current(), __VA_ARGS__)
#define LOG_INFO(sc, ...)     ::terila::log::logf(::terila::log::Level::Info,     ::terila::log::Scope::sc, std::source_location::current(), __VA_ARGS__)
#define LOG_WARN(sc, ...)     ::terila::log::logf(::terila::log::Level::Warn,     ::terila::log::Scope::sc, std::source_location::current(), __VA_ARGS__)
#define LOG_ERROR(sc, ...)    ::terila::log::logf(::terila::log::Level::Error,    ::terila::log::Scope::sc, std::source_location::current(), __VA_ARGS__)
#define LOG_CRITICAL(sc, ...) ::terila::log::logf(::terila::log::Level::Critical, ::terila::log::Scope::sc, std::source_location::current(), __VA_ARGS__)
#define LOG_FATAL(sc, ...)    ::terila::log::logf(::terila::log::Level::Fatal,    ::terila::log::Scope::sc, std::source_location::current(), __VA_ARGS__)