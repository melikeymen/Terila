#pragma once

#include "log/logger.hpp"
#include <fstream>
#include <string>
#include <cstdint>
#include <algorithm>
#include <cctype>

namespace terila::config {

    struct ServerConfig {
        std::string   bind_address = "0.0.0.0";
        std::uint16_t port = 25565;
        std::string   log_level = "info";
        std::string   log_file = "";
    };

    namespace detail {

        inline std::string trim(const std::string& s) {
            auto start = s.begin();
            while (start != s.end() && std::isspace(static_cast<unsigned char>(*start))) ++start;
            auto end = s.end();
            while (end != start && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
            return std::string(start, end);
        }

    }

    inline bool load(ServerConfig& out, const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;

        std::string line;
        while (std::getline(f, line)) {
            line = detail::trim(line);
            if (line.empty() || line[0] == '#') continue;

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = detail::trim(line.substr(0, eq));
            std::string value = detail::trim(line.substr(eq + 1));

            if (key == "bind-address") {
                out.bind_address = value;
            }
            else if (key == "server-port") {
                try {
                    out.port = static_cast<std::uint16_t>(std::stoul(value));
                }
                catch (...) {}
            }
            else if (key == "log-level") {
                out.log_level = value;
            }
            else if (key == "log-file") {
                out.log_file = value;
            }
        }

        return true;
    }

}