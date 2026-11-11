#pragma once

#include "log/logger.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <string>

namespace terila::protocol {

    namespace detail {

        // OpenSSL provides the digest; the server carries no crypto implementation of its own.
        inline std::array<std::uint8_t, 16> md5(const std::string& input) {
            std::array<std::uint8_t, 16> digest{};
            unsigned int length = 0;

            if (EVP_Digest(input.data(), input.size(), digest.data(), &length, EVP_md5(), nullptr) != 1 ||
                length != digest.size()) {
                LOG_WARN(Protocol, "md5: EVP_Digest failed");
                return {};
            }

            return digest;
        }

    }

    // Vanilla offline mode: UUID v3 of "OfflinePlayer:<name>".
    inline std::array<std::uint8_t, 16> offline_uuid(const std::string& username) {
        auto digest = detail::md5("OfflinePlayer:" + username);
        digest[6] = static_cast<std::uint8_t>((digest[6] & 0x0F) | 0x30);
        digest[8] = static_cast<std::uint8_t>((digest[8] & 0x3F) | 0x80);
        return digest;
    }

    inline std::string uuid_to_string(const std::array<std::uint8_t, 16>& uuid) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(36);
        for (std::size_t i = 0; i < uuid.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                out.push_back('-');
            }
            out.push_back(hex[(uuid[i] >> 4) & 0x0F]);
            out.push_back(hex[uuid[i] & 0x0F]);
        }
        return out;
    }

} // namespace terila::protocol
