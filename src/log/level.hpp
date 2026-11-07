#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace terila::log {

    enum class Level : std::uint8_t {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
        Critical = 5,
        Fatal = 6,
    };

    enum class Scope : std::uint8_t {
        Main = 0,
        Network = 1,
        Protocol = 2,
        Data = 3,
        Registry = 4,
        Config = 5,
        Play = 6,
        Chunk = 7,
    };

    namespace detail {

        inline char lower(char c) noexcept {
            if (c >= 'A' && c <= 'Z') return static_cast<char>(c + ('a' - 'A'));
            return c;
        }

        inline bool iequals(std::string_view a, std::string_view b) noexcept {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (lower(a[i]) != lower(b[i])) return false;
            }
            return true;
        }

    }

    inline std::optional<Level> parse_level(std::string_view s) {
        if (detail::iequals(s, "trace"))    return Level::Trace;
        if (detail::iequals(s, "debug"))    return Level::Debug;
        if (detail::iequals(s, "info"))     return Level::Info;
        if (detail::iequals(s, "warn"))     return Level::Warn;
        if (detail::iequals(s, "error"))    return Level::Error;
        if (detail::iequals(s, "critical")) return Level::Critical;
        if (detail::iequals(s, "fatal"))    return Level::Fatal;
        return std::nullopt;
    }

    inline std::optional<Scope> parse_scope(std::string_view s) {
        if (detail::iequals(s, "main"))     return Scope::Main;
        if (detail::iequals(s, "network"))  return Scope::Network;
        if (detail::iequals(s, "protocol")) return Scope::Protocol;
        if (detail::iequals(s, "data"))     return Scope::Data;
        if (detail::iequals(s, "registry")) return Scope::Registry;
        if (detail::iequals(s, "config"))   return Scope::Config;
        if (detail::iequals(s, "play"))     return Scope::Play;
        if (detail::iequals(s, "chunk"))    return Scope::Chunk;
        return std::nullopt;
    }

    inline std::string_view level_name(Level lvl) noexcept {
        switch (lvl) {
        case Level::Trace:    return "TRACE";
        case Level::Debug:    return "DEBUG";
        case Level::Info:     return "INFO";
        case Level::Warn:     return "WARN";
        case Level::Error:    return "ERROR";
        case Level::Critical: return "CRITICAL";
        case Level::Fatal:    return "FATAL";
        }
        return "UNKNOWN";
    }

    inline std::string_view scope_name(Scope sc) noexcept {
        switch (sc) {
        case Scope::Main:     return "MAIN";
        case Scope::Network:  return "NETWORK";
        case Scope::Protocol: return "PROTOCOL";
        case Scope::Data:     return "DATA";
        case Scope::Registry: return "REGISTRY";
        case Scope::Config:   return "CONFIG";
        case Scope::Play:     return "PLAY";
        case Scope::Chunk:    return "CHUNK";
        }
        return "UNKNOWN";
    }

} // namespace terila::log