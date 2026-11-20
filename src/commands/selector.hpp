#pragma once

// Entity selectors, shared by every command: @s, @p, @r, @a, @e and plain player names,
// with the argument list the game uses (@a[distance=..10,limit=2,sort=nearest,name=Alex]).
//
// A command never decides on its own what @a means; it hands the parsed selector and the
// players the caller knows about to resolve(), which applies the marker, the filters, the
// sort order and the limit in one place.

#include "commands/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace terila::commands {

    // A player a selector can match, with the position distance= and sort= are measured
    // from.
    struct Target {
        std::int32_t entity = 0;
        std::string name;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        // Facing, used by /tp <destination> and by execute at/rotated.
        float yaw = 0.0f;
        float pitch = 0.0f;
    };

    enum class Sort { Arbitrary, Nearest, Furthest, Random };

    struct Selector {
        std::string text;
        char marker = 0;      // '@' letter, or 0 for a player name
        std::string name;     // literal name when marker is 0
        bool single = false;  // @s, @p, @r and names pick one target
        int limit = 0;        // 0: whatever the marker implies
        Sort sort = Sort::Arbitrary;
        bool has_distance = false;
        double distance_min = 0.0;
        double distance_max = 0.0;
        std::string name_filter;
    };

    inline bool is_selector(std::string_view text) {
        return !text.empty() && text.front() == '@';
    }

    // "8", "..8", "8.." or "4..12", the way the game writes distances.
    inline bool parse_distance(std::string_view text, double& min, double& max) {
        const auto dots = text.find("..");
        try {
            if (dots == std::string_view::npos) {
                const double value = std::stod(std::string(text));
                min = value;
                max = value;
                return true;
            }
            const std::string_view first = text.substr(0, dots);
            const std::string_view last = text.substr(dots + 2);
            min = first.empty() ? 0.0 : std::stod(std::string(first));
            max = last.empty() ? 1.0e9 : std::stod(std::string(last));
            return true;
        }
        catch (...) {
            return false;
        }
    }

    // Returns nothing for a marker this server does not know, a broken argument list or an
    // option it cannot honour: a selector that silently ignores half of itself would pick
    // the wrong players.
    inline std::optional<Selector> parse(std::string_view text) {
        Selector selector;
        std::string_view rest = trim(text);
        if (rest.empty()) {
            return std::nullopt;
        }
        selector.text = std::string(rest);

        if (rest.front() == '@') {
            if (rest.size() < 2) {
                return std::nullopt;
            }
            selector.marker = static_cast<char>(std::tolower(static_cast<unsigned char>(rest[1])));
            switch (selector.marker) {
            case 's':
            case 'p':
            case 'r':
                selector.single = true;
                break;
            case 'a':
            case 'e':
                break;
            default:
                return std::nullopt;
            }
            rest.remove_prefix(2);
        }
        else {
            const auto bracket = rest.find('[');
            selector.name = lower(rest.substr(0, bracket));
            if (selector.name.empty()) {
                return std::nullopt;
            }
            selector.single = true;
            rest = bracket == std::string_view::npos ? std::string_view{} : rest.substr(bracket);
        }

        rest = trim(rest);
        if (rest.empty()) {
            return selector;
        }
        if (rest.front() != '[' || rest.back() != ']') {
            return std::nullopt;
        }
        rest = rest.substr(1, rest.size() - 2);

        while (true) {
            const auto comma = rest.find(',');
            const std::string_view option = trim(rest.substr(0, comma));
            if (!option.empty()) {
                const auto eq = option.find('=');
                if (eq == std::string_view::npos) {
                    return std::nullopt;
                }
                const std::string key = lower(trim(option.substr(0, eq)));
                const std::string_view value = trim(option.substr(eq + 1));

                if (key == "limit") {
                    try {
                        selector.limit = std::max(1, std::stoi(std::string(value)));
                    }
                    catch (...) {
                        return std::nullopt;
                    }
                }
                else if (key == "sort") {
                    const std::string order = lower(value);
                    if (order == "nearest") selector.sort = Sort::Nearest;
                    else if (order == "furthest") selector.sort = Sort::Furthest;
                    else if (order == "random") selector.sort = Sort::Random;
                    else if (order == "arbitrary") selector.sort = Sort::Arbitrary;
                    else return std::nullopt;
                }
                else if (key == "distance") {
                    if (!parse_distance(value, selector.distance_min, selector.distance_max)) {
                        return std::nullopt;
                    }
                    selector.has_distance = true;
                }
                else if (key == "name") {
                    selector.name_filter = std::string(value);
                }
                else {
                    return std::nullopt;
                }
            }

            if (comma == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(comma + 1);
        }
        return selector;
    }

    // Applies the marker, the filters, the sort order and the limit. @s is the caller, @p
    // the nearest candidate, @r one at random, @a and @e every candidate, and a name the
    // player called that.
    inline std::vector<Target> resolve(const Selector& selector, const Target& self,
        const std::vector<Target>& players) {
        // xorshift: /give @r should not hand items to the same players in the same order
        // every run.
        static thread_local std::uint64_t state = 0x9E3779B97F4A7C15ull;
        const auto next = []() {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        };

        const auto distance_sq = [&self](const Target& target) {
            const double dx = target.x - self.x;
            const double dy = target.y - self.y;
            const double dz = target.z - self.z;
            return dx * dx + dy * dy + dz * dz;
        };

        std::vector<Target> matches;
        matches.reserve(players.size());
        for (const auto& target : players) {
            if (!selector.name_filter.empty() &&
                !equal_ignore_case(target.name, selector.name_filter)) {
                continue;
            }
            if (selector.has_distance) {
                const double distance = std::sqrt(distance_sq(target));
                if (distance < selector.distance_min || distance > selector.distance_max) {
                    continue;
                }
            }
            matches.push_back(target);
        }

        if (selector.marker == 0) {
            for (const auto& target : matches) {
                if (equal_ignore_case(target.name, selector.name)) {
                    return { target };
                }
            }
            return {};
        }
        if (selector.marker == 's') {
            for (const auto& target : matches) {
                if (target.entity == self.entity) {
                    return { target };
                }
            }
            return {};
        }

        Sort sort = selector.sort;
        if (sort == Sort::Arbitrary && selector.marker == 'p') {
            sort = Sort::Nearest;
        }
        if (sort == Sort::Arbitrary && selector.marker == 'r') {
            sort = Sort::Random;
        }
        switch (sort) {
        case Sort::Nearest:
            std::stable_sort(matches.begin(), matches.end(),
                [&](const Target& a, const Target& b) { return distance_sq(a) < distance_sq(b); });
            break;
        case Sort::Furthest:
            std::stable_sort(matches.begin(), matches.end(),
                [&](const Target& a, const Target& b) { return distance_sq(a) > distance_sq(b); });
            break;
        case Sort::Random:
            for (std::size_t i = matches.size(); i > 1; --i) {
                std::swap(matches[i - 1], matches[next() % i]);
            }
            break;
        case Sort::Arbitrary:
            break;
        }

        int limit = selector.limit;
        if (limit <= 0) {
            limit = selector.single ? 1 : 0;
        }
        if (limit > 0 && matches.size() > static_cast<std::size_t>(limit)) {
            matches.resize(static_cast<std::size_t>(limit));
        }
        return matches;
    }

} // namespace terila::commands
