#pragma once

#include "nbt/nbt_text.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/string.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace terila::protocol {

    namespace detail {

        // The login reason travels as a JSON component, so a quote in the text would end the
        // string early and turn the packet into garbage the client cannot parse.
        inline void append_json_string(std::string& out, std::string_view text) {
            for (const char c : text) {
                switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char escape[8];
                        std::snprintf(escape, sizeof(escape), "\\u%04x",
                            static_cast<unsigned int>(static_cast<unsigned char>(c)));
                        out += escape;
                    }
                    else {
                        out.push_back(c);
                    }
                    break;
                }
            }
        }

    } // namespace detail

    // A refused login: the reason is a JSON text component here and the client shows it on
    // its disconnect screen instead of waiting for a login that will never come.
    inline bool write_login_disconnect(std::vector<std::uint8_t>& output, std::string_view reason) {
        std::string json = "{\"text\":\"";
        detail::append_json_string(json, reason);
        json += "\"}";

        std::vector<std::uint8_t> payload;
        if (!write_string(payload, json)) {
            LOG_WARN(Protocol, "write_login_disconnect: reason does not fit in a string");
            return false;
        }
        if (!write_packet(output, packet_ids().login_disconnect, payload)) {
            LOG_WARN(Protocol, "write_login_disconnect: failed");
            return false;
        }
        return true;
    }

    // Configuration and play carry the binary tag form of the same component, which is the
    // shape the chat packets in those states already use.
    inline bool write_component_disconnect(std::vector<std::uint8_t>& output,
        std::int32_t packet_id, std::string_view reason) {
        if (packet_id < 0) {
            // No id means the dump this build loaded has no such packet; sending it on an
            // invented number would be worse than dropping the connection quietly.
            LOG_WARN(Protocol, "write_component_disconnect: no packet id for the disconnect");
            return false;
        }

        std::vector<std::uint8_t> payload = terila::nbt::text_component_nbt(reason);
        if (!write_packet(output, packet_id, payload)) {
            LOG_WARN(Protocol, "write_component_disconnect: failed");
            return false;
        }
        return true;
    }

} // namespace terila::protocol
