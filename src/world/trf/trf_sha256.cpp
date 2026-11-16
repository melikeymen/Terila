#include "world/trf/trf_sha256.hpp"

#include <openssl/evp.h>

namespace terila::world::trf {

    std::optional<Sha256Digest> sha256(std::span<const std::byte> data) noexcept {
        Sha256Digest digest{};
        unsigned int length = 0;

        EVP_MD_CTX* context = EVP_MD_CTX_new();
        if (context == nullptr) {
            return std::nullopt;
        }

        if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
            EVP_DigestUpdate(context, data.data(), data.size()) != 1 ||
            EVP_DigestFinal_ex(context, reinterpret_cast<unsigned char*>(digest.data()), &length) != 1 ||
            length != digest.size()) {
            EVP_MD_CTX_free(context);
            return std::nullopt;
        }

        EVP_MD_CTX_free(context);
        return digest;
    }

}
