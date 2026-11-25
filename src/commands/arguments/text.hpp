#pragma once

// Text handling shared by the command modules: trimming, the next whitespace-delimited
// token, lowercasing and case-insensitive comparison. Commands split arguments through
// these instead of each writing its own copy.

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace terila::commands {

    inline std::string_view trim(std::string_view text) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
            text.remove_prefix(1);
        }
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
            text.remove_suffix(1);
        }
        return text;
    }

    // The next word, with `rest` left after it.
    inline std::string_view token(std::string_view text, std::string_view& rest) {
        text = trim(text);
        std::size_t end = 0;
        while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) {
            ++end;
        }
        rest = text.substr(end);
        return text.substr(0, end);
    }

    inline std::string lower(std::string_view text) {
        std::string out(text);
        for (char& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    inline bool equal_ignore_case(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

} // namespace terila::commands
