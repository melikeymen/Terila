#pragma once

// The closed list of things a plugin can be granted, and the set arithmetic the host does
// with it. A plugin asks for capabilities in its package; the operator grants them in
// config/plugin-grants/<id>.json; what the plugin actually holds is the intersection.
//
// Names are group.verb and there are no wildcards, so a grant file can be read and
// understood without the host's source in front of you. Adding a name here is a deliberate
// act: it is the moment the host promises to enforce something new.

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace terila::plugin {

    inline constexpr std::array<std::string_view, 11> kCapabilities{
        "log.write",
        "chat.send",
        "chat.receive",
        "command.register",
        "player.read",
        "player.teleport",
        "player.kick",
        "world.read",
        "world.write",
        "packet.read",
        "packet.cancel",
    };

    inline bool is_known_capability(std::string_view name) noexcept {
        for (const std::string_view candidate : kCapabilities) {
            if (candidate == name) return true;
        }
        return false;
    }

    // The first name the registry does not know, so a refusal can quote it. Empty when every
    // name is known.
    inline std::string_view first_unknown_capability(
        const std::vector<std::string>& names) noexcept {
        for (const std::string& name : names) {
            if (!is_known_capability(name)) return name;
        }
        return {};
    }

    // True when every name in `requested` also appears in `granted`. Duplicates are ignored.
    inline bool is_subset(const std::vector<std::string>& requested,
        const std::vector<std::string>& granted) noexcept {
        for (const std::string& name : requested) {
            bool found = false;
            for (const std::string& allowed : granted) {
                if (allowed == name) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }

    // What a plugin may actually do: the requested names the grant also lists, in the order
    // the request gave them. Unknown names on either side are dropped, so a grant that
    // mentions a capability this build does not have cannot widen anything.
    inline std::vector<std::string> effective_capabilities(
        const std::vector<std::string>& requested, const std::vector<std::string>& granted) {
        std::vector<std::string> effective;
        for (const std::string& name : requested) {
            if (!is_known_capability(name)) continue;
            if (!is_subset({ name }, granted)) continue;
            bool seen = false;
            for (const std::string& present : effective) {
                if (present == name) {
                    seen = true;
                    break;
                }
            }
            if (!seen) effective.push_back(name);
        }
        return effective;
    }

    // A plugin id names a directory, a grant file and every log line about it, so it is kept
    // to characters that survive all three without quoting.
    inline bool is_valid_plugin_id(std::string_view id) noexcept {
        if (id.size() < 3 || id.size() > 64) return false;
        if (id.front() == '.' || id.back() == '.' || id.back() == '-') return false;
        for (std::size_t i = 0; i < id.size(); ++i) {
            const char c = id[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '_' || c == '-';
            if (!ok) return false;
            if (c == '.' && i + 1 < id.size() && id[i + 1] == '.') return false;
        }
        return true;
    }

} // namespace terila::plugin
