#pragma once

#include "protocol/player_info.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace terila::network {

    struct PlayerProfile {
        // The account's real (Mojang) UUID, which is what the game and the skin service
        // use. It is not derivable from the name, so it has to be looked up.
        std::array<std::uint8_t, 16> uuid{};
        std::string name;
        // The signed "textures" property; the client only shows a skin of the player list
        // entry that carries it.
        std::vector<protocol::ProfileProperty> properties;
        bool found = false;
    };

    // Asks Mojang for the account behind a name. Results are cached, and a name that does
    // not resolve (no connection, unknown account) comes back as not found so the caller can
    // fall back to the offline UUID instead of failing the login.
    PlayerProfile lookup_mojang_profile(const std::string& name);

    // Forgets a cached account, for when a player changes their skin.
    void forget_mojang_profile(const std::string& name);

} // namespace terila::network
