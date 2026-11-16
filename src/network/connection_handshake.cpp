#include "network/connection.hpp"

#include "protocol/handshake.hpp"
#include "protocol/status.hpp"

#include "version/mc_version.hpp"

namespace terila::network {

    void Connection::handle_handshake(std::int32_t id, const std::vector<std::uint8_t>& payload) {
        (void)id;
        (void)payload;

        auto hs = terila::protocol::read_handshake(read_accum_.data(), read_accum_.size());

        if (!hs.has_value()) {
            LOG_WARN(Network, "invalid handshake packet");
            return;
        }

        LOG_DEBUG(Network, "handshake: protocol={} address={} port={} next_state={}",
            hs->protocol, hs->address, hs->port, hs->next_state);

        if (hs->next_state == 1) {
            state_ = State::Status;
            LOG_DEBUG(Network, "state: Handshake → Status");
        }
        else if (hs->next_state == 2) {
            state_ = State::Login;
            LOG_DEBUG(Network, "state: Handshake → Login");
        }
        else {
            LOG_WARN(Network, "invalid next_state={} in handshake", hs->next_state);
        }
    }

    void Connection::handle_status(std::int32_t id, const std::vector<std::uint8_t>& payload) {
        (void)payload;

        const auto& ids = terila::protocol::packet_ids();

        if (id == ids.status_request) {
            if (!terila::protocol::read_status_request(read_accum_.data(), read_accum_.size())) {
                LOG_WARN(Network, "invalid status request");
                return;
            }

            terila::protocol::StatusResponse resp;
            // Both numbers come from the compiled protocol target, so a status reply can
            // never advertise a version the build does not speak.
            resp.version_name = std::string(terila::version::name());
            resp.version_protocol = terila::version::protocol();
            resp.players_max = max_players_;
            resp.players_online = static_cast<std::int32_t>(hub_ ? hub_->count() : 0);
            resp.description = "Terila Server";
            resp.favicon = "";

            std::vector<std::uint8_t> out;
            if (terila::protocol::write_status_response(out, resp)) {
                send(out);
                LOG_DEBUG(Network, "status response sent ({} bytes)", out.size());
                return;
            }
            LOG_ERROR(Network, "write_status_response failed");
        }
        else if (id == ids.ping) {
            auto ping_value = terila::protocol::read_ping(read_accum_.data(), read_accum_.size());
            if (!ping_value.has_value()) {
                LOG_WARN(Network, "invalid ping packet");
                return;
            }

            std::vector<std::uint8_t> out;
            terila::protocol::write_pong(out, *ping_value);
            send(out);
            LOG_TRACE(Network, "pong sent, value={}", *ping_value);
        }
        else {
            LOG_DEBUG(Network, "unknown status packet id=0x{:02X}", id);
        }
    }

} // namespace terila::network
