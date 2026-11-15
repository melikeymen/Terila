#include "network/mojang_profile.hpp"

#include "log/logger.hpp"
#include "protocol/uuid.hpp"

#include <nlohmann/json.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace terila::network {

    namespace {

#ifdef _WIN32
        using Socket = SOCKET;
        constexpr Socket kInvalidSocket = INVALID_SOCKET;

        void close_socket(Socket socket) { closesocket(socket); }

        // asio normally does this for the server, but the lookup has to work on its own.
        void start_sockets() {
            static std::once_flag once;
            std::call_once(once, [] {
                WSADATA data{};
                WSAStartup(MAKEWORD(2, 2), &data);
            });
        }
#else
        using Socket = int;
        constexpr Socket kInvalidSocket = -1;

        void close_socket(Socket socket) { ::close(socket); }

        void start_sockets() {}
#endif

        // Nothing here may stall the login for long: Mojang is either there in a moment or
        // the player gets the offline identity.
        constexpr int kTimeoutMs = 2500;
        const auto kCacheTtl = std::chrono::minutes(30);

        bool wait_for(Socket socket, bool writing) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            timeval timeout{};
            timeout.tv_sec = kTimeoutMs / 1000;
            timeout.tv_usec = (kTimeoutMs % 1000) * 1000;
            const int ready = ::select(static_cast<int>(socket) + 1,
                writing ? nullptr : &set, writing ? &set : nullptr, nullptr, &timeout);
            return ready > 0;
        }

        Socket connect_to(const std::string& host, std::string& error) {
            start_sockets();

            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* found = nullptr;
            if (::getaddrinfo(host.c_str(), "443", &hints, &found) != 0 || found == nullptr) {
                error = "cannot resolve " + host;
                return kInvalidSocket;
            }

            Socket result = kInvalidSocket;
            for (addrinfo* it = found; it != nullptr; it = it->ai_next) {
                const Socket socket = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
                if (socket == kInvalidSocket) {
                    continue;
                }

#ifdef _WIN32
                u_long non_blocking = 1;
                ioctlsocket(socket, FIONBIO, &non_blocking);
#else
                const int flags = ::fcntl(socket, F_GETFL, 0);
                ::fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
                const int connected = ::connect(socket, it->ai_addr,
                    static_cast<int>(it->ai_addrlen));
                if (connected == 0) {
                    result = socket;
                    break;
                }
                if (wait_for(socket, true)) {
                    int so_error = 0;
                    socklen_t length = sizeof(so_error);
                    ::getsockopt(socket, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char*>(&so_error), &length);
                    if (so_error == 0) {
                        result = socket;
                        break;
                    }
                }
                close_socket(socket);
            }
            ::freeaddrinfo(found);

            if (result == kInvalidSocket) {
                error = "cannot reach " + host;
            }
            return result;
        }

        // Removes the HTTP framing so the caller sees the JSON body only.
        std::string http_body(const std::string& raw) {
            const auto header_end = raw.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                return {};
            }
            const std::string headers = raw.substr(0, header_end);
            std::string body = raw.substr(header_end + 4);

            std::string lower = headers;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("transfer-encoding: chunked") == std::string::npos) {
                return body;
            }

            std::string decoded;
            std::size_t pos = 0;
            while (pos < body.size()) {
                const auto line_end = body.find("\r\n", pos);
                if (line_end == std::string::npos) {
                    break;
                }
                const std::string size_text = body.substr(pos, line_end - pos);
                std::size_t size = 0;
                try {
                    size = std::stoul(size_text, nullptr, 16);
                }
                catch (...) {
                    break;
                }
                pos = line_end + 2;
                if (size == 0) {
                    break;
                }
                if (pos + size > body.size()) {
                    size = body.size() - pos;
                }
                decoded.append(body, pos, size);
                pos += size + 2;
            }
            return decoded;
        }

        std::optional<std::string> https_get(const std::string& host, const std::string& path) {
            std::string error;
            const Socket socket = connect_to(host, error);
            if (socket == kInvalidSocket) {
                LOG_WARN(Network, "mojang lookup failed: {}", error);
                return std::nullopt;
            }

            SSL_CTX* context = SSL_CTX_new(TLS_client_method());
            if (context == nullptr) {
                close_socket(socket);
                return std::nullopt;
            }
            SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);

            SSL* ssl = SSL_new(context);
            SSL_set_tlsext_host_name(ssl, host.c_str());
            SSL_set_fd(ssl, static_cast<int>(socket));
            SSL_set_connect_state(ssl);

            const auto step = [&](int result) {
                const int failure = SSL_get_error(ssl, result);
                return failure == SSL_ERROR_WANT_READ ? 1
                    : failure == SSL_ERROR_WANT_WRITE ? 2
                    : 0;
            };

            bool ok = false;
            while (true) {
                const int result = SSL_connect(ssl);
                if (result == 1) {
                    ok = true;
                    break;
                }
                const int want = step(result);
                if (want == 0 || !wait_for(socket, want == 2)) {
                    break;
                }
            }

            std::string raw;
            if (ok) {
                const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                    "\r\nUser-Agent: terila\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
                std::size_t written = 0;
                while (written < request.size()) {
                    const int result = SSL_write(ssl,
                        request.data() + written, static_cast<int>(request.size() - written));
                    if (result > 0) {
                        written += static_cast<std::size_t>(result);
                        continue;
                    }
                    const int want = step(result);
                    if (want == 0 || !wait_for(socket, want == 2)) {
                        break;
                    }
                }

                char buffer[4096];
                while (written == request.size() && raw.size() < 65536) {
                    const int result = SSL_read(ssl, buffer, static_cast<int>(sizeof(buffer)));
                    if (result > 0) {
                        raw.append(buffer, static_cast<std::size_t>(result));
                        continue;
                    }
                    const int want = step(result);
                    if (want == 0 || !wait_for(socket, want == 2)) {
                        break;
                    }
                }
            }

            SSL_free(ssl);
            SSL_CTX_free(context);
            close_socket(socket);

            if (!ok) {
                LOG_WARN(Network, "mojang lookup failed: TLS handshake with {}", host);
                return std::nullopt;
            }
            if (!raw.empty() && raw.rfind("HTTP/1.1 200", 0) != 0 && raw.rfind("HTTP/1.0 200", 0) != 0) {
                return std::nullopt;   // 404 for an unknown name, 429 when we ask too often
            }
            return http_body(raw);
        }

        std::string url_encode(const std::string& text) {
            static constexpr char hex[] = "0123456789ABCDEF";
            std::string out;
            for (const unsigned char c : text) {
                if (std::isalnum(c) != 0 || c == '_' || c == '-') {
                    out.push_back(static_cast<char>(c));
                }
                else {
                    out.push_back('%');
                    out.push_back(hex[(c >> 4) & 0x0F]);
                    out.push_back(hex[c & 0x0F]);
                }
            }
            return out;
        }

        std::string cache_key(const std::string& name) {
            std::string key = name;
            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return key;
        }

        struct CacheEntry {
            PlayerProfile profile;
            std::chrono::steady_clock::time_point at;
        };

        std::unordered_map<std::string, CacheEntry>& cache() {
            static std::unordered_map<std::string, CacheEntry> entries;
            return entries;
        }

        std::mutex& cache_mutex() {
            static std::mutex lock;
            return lock;
        }

        const std::vector<protocol::ProfileProperty>& read_properties(
            const nlohmann::json& profile_json) {
            static const std::vector<protocol::ProfileProperty> empty;
            if (!profile_json.contains("properties") || !profile_json["properties"].is_array()) {
                return empty;
            }
            // The caller stores the result; the static keeps the signature simple.
            static thread_local std::vector<protocol::ProfileProperty> parsed;
            parsed.clear();
            for (const auto& property : profile_json["properties"]) {
                if (!property.contains("name") || !property["name"].is_string() ||
                    !property.contains("value") || !property["value"].is_string()) {
                    continue;
                }
                protocol::ProfileProperty entry;
                entry.name = property["name"].get<std::string>();
                entry.value = property["value"].get<std::string>();
                if (property.contains("signature") && property["signature"].is_string()) {
                    entry.signature = property["signature"].get<std::string>();
                    entry.signed_value = !entry.signature.empty();
                }
                parsed.push_back(std::move(entry));
            }
            return parsed;
        }

    } // namespace

    PlayerProfile lookup_mojang_profile(const std::string& name) {
        const std::string key = cache_key(name);

        {
            std::lock_guard lock(cache_mutex());
            const auto it = cache().find(key);
            if (it != cache().end() && std::chrono::steady_clock::now() - it->second.at < kCacheTtl) {
                return it->second.profile;
            }
        }

        PlayerProfile profile;
        profile.name = name;

        // The old name lookup answers with the UUID and the current spelling of the name;
        // the session server then hands out the signed skin texture.
        if (const auto account = https_get("api.mojang.com",
                "/users/profiles/minecraft/" + url_encode(name))) {
            const auto json = nlohmann::json::parse(*account, nullptr, false);
            if (!json.is_discarded() && json.contains("id") && json["id"].is_string()) {
                const std::string hex = json["id"].get<std::string>();
                if (const auto uuid = protocol::uuid_from_hex(hex)) {
                    profile.uuid = *uuid;
                    profile.found = true;
                    if (json.contains("name") && json["name"].is_string()) {
                        profile.name = json["name"].get<std::string>();
                    }

                    // The session server only answers the dashed form; the bare hex one
                    // comes back as an HTML error page.
                    if (const auto session = https_get("sessionserver.mojang.com",
                            "/session/minecraft/profile/" +
                                protocol::uuid_to_string(*uuid))) {
                        const auto session_json = nlohmann::json::parse(*session, nullptr, false);
                        if (!session_json.is_discarded()) {
                            const auto& properties = read_properties(session_json);
                            profile.properties.assign(properties.begin(), properties.end());
                        }
                    }
                }
            }
        }

        if (profile.found) {
            LOG_INFO(Network, "mojang: {} is {} with {} profile properties", name,
                protocol::uuid_to_string(profile.uuid), profile.properties.size());
        }
        else {
            LOG_WARN(Network, "mojang: no account for '{}', falling back to the offline UUID", name);
        }

        {
            std::lock_guard lock(cache_mutex());
            cache()[key] = CacheEntry{ profile, std::chrono::steady_clock::now() };
        }
        return profile;
    }

    void forget_mojang_profile(const std::string& name) {
        std::lock_guard lock(cache_mutex());
        cache().erase(cache_key(name));
    }

} // namespace terila::network
