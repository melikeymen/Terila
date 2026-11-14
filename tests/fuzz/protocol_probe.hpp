#pragma once

#include "protocol/chat.hpp"
#include "protocol/handshake.hpp"
#include "protocol/login.hpp"
#include "protocol/menu.hpp"
#include "protocol/packet.hpp"
#include "protocol/primitives.hpp"
#include "protocol/status.hpp"
#include "protocol/string.hpp"
#include "protocol/varint.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace terila::test {

    // Feeds arbitrary bytes to every serverbound reader. They may reject anything, but
    // they must never read outside the buffer or crash.
    inline void probe_protocol(const std::uint8_t* data, std::size_t size) {
        namespace protocol = terila::protocol;

        (void)protocol::read_varint(data, size);
        (void)protocol::read_string(data, size);
        (void)protocol::read_uint16(data, size);
        (void)protocol::read_int64(data, size);
        (void)protocol::read_double(data, size);

        (void)protocol::read_packet(data, size);
        (void)protocol::read_handshake(data, size);
        (void)protocol::read_login_start(data, size);
        (void)protocol::read_status_request(data, size);
        (void)protocol::read_ping(data, size);
        (void)protocol::read_status_response(data, size);
        (void)protocol::read_container_click(data, size);

        const std::vector<std::uint8_t> payload(data, data + size);
        (void)protocol::read_chat_command(payload);
    }

} // namespace terila::test
