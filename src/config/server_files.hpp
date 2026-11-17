#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace terila::config {

    // Mirror of ServerConfig's defaults; the file is only written when it is missing, so
    // an existing configuration is never overwritten.
    inline constexpr const char* kDefaultProperties =
        "bind-address=0.0.0.0\n"
        "server-port=25565\n"
        "log-level=info\n"
        "log-file=\n"
        "\n"
        "# Lowest log level that shuts the server down: never, fatal (default), critical,\n"
        "# error or warn.\n"
        "log-exit-level=fatal\n"
        "\n"
        "# World generation seed. Numbers are used as-is, other text is hashed.\n"
        "level-seed=\n"
        "\n"
        "# World generator. normal streams the terrain (oceans, hills, biomes, trees),\n"
        "# flat streams the classic superflat layer cake.\n"
        "level-type=normal\n"
        "\n"
        "# Mojang session authentication. false lets cracked (\"offline\") clients join.\n"
        "online-mode=false\n"
        "\n"
        "# Encrypt the connection with AES/CFB8 after an RSA key exchange. Independent from\n"
        "# online-mode: cracked clients can be encrypted too.\n"
        "network-encryption=false\n"
        "\n"
        "# Chunk radius sent around the player, in chunks (vanilla default 10).\n"
        "view-distance=10\n"
        "\n"
        "# Radius used for ticking.\n"
        "simulation-distance=10\n"
        "\n"
        "# Advertised player limit and the gamemode new players join with.\n"
        "max-players=20\n"
        "gamemode=1\n";

    // eula.txt is Minecraft's acceptance marker: true starts the server, false refuses.
    inline constexpr const char* kDefaultEula =
        "# By setting eula=true you confirm you accept the Minecraft EULA\n"
        "# (https://aka.ms/MinecraftEULA). eula=false makes the server refuse to start.\n"
        "eula=true\n";

    // A first run has neither file, and failing with \"input not found\" because a config
    // file is absent is a bad first impression, so both are written with their defaults.
    inline bool ensure_default_file(const std::filesystem::path& path, const char* contents,
        bool* created = nullptr) {
        if (created != nullptr) {
            *created = false;
        }

        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) {
            return true;
        }

        std::ofstream out(path, std::ios::binary);
        if (!out) {
            return false;
        }
        out << contents;
        if (created != nullptr && out.good()) {
            *created = true;
        }
        return out.good();
    }

    // Only an explicit eula=false refuses to start; the file we write says true.
    inline bool eula_accepted(const std::filesystem::path& path) {
        std::ifstream in(path);
        if (!in) {
            return false;
        }

        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            const auto trim = [](std::string& text) {
                text.erase(text.begin(), std::find_if(text.begin(), text.end(),
                    [](unsigned char c) { return !std::isspace(c); }));
                text.erase(std::find_if(text.rbegin(), text.rend(),
                    [](unsigned char c) { return !std::isspace(c); }).base(), text.end());
                for (char& c : text) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            };
            trim(key);
            trim(value);

            if (key != "eula") {
                continue;
            }
            return value != "false" && value != "0" && value != "no" && value != "off";
        }
        return true;
    }

} // namespace terila::config
