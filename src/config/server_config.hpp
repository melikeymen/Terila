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

        // Mojang session authentication. false lets cracked (offline) clients join.
        bool          online_mode = false;

        // AES/CFB8 encryption after the RSA exchange. Independent from online_mode:
        // offline clients can be encrypted too.
        bool          network_encryption = false;

        // Chunk radius streamed around the player, in chunks.
        int           view_distance = 10;

        // Radius used for ticking; parsed only while the server has no entities.
        int           simulation_distance = 10;
    };

    namespace detail {

        inline int parse_distance(const std::string& value, int fallback, int min_value, int max_value) {
            try {
                const int parsed = std::stoi(value);
                return std::min(std::max(parsed, min_value), max_value);
            }
            catch (...) {
                return fallback;
            }
        }

        inline bool parse_bool(const std::string& value, bool fallback) {
            std::string lowered;
            lowered.reserve(value.size());
            for (char c : value) {
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }

            if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") return true;
            if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") return false;
            return fallback;
        }

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
            else if (key == "online-mode") {
                out.online_mode = detail::parse_bool(value, out.online_mode);
            }
            else if (key == "network-encryption") {
                out.network_encryption = detail::parse_bool(value, out.network_encryption);
            }
            else if (key == "view-distance") {
                out.view_distance = detail::parse_distance(value, out.view_distance, 2, 32);
            }
            else if (key == "simulation-distance") {
                out.simulation_distance = detail::parse_distance(value, out.simulation_distance, 1, 32);
            }
        }

        return true;
    }

}