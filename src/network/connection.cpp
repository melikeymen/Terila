#include "network/connection.hpp"

#include "protocol/handshake.hpp"
#include "protocol/login.hpp"
#include "protocol/packet.hpp"
#include "protocol/packet_ids.hpp"
#include "protocol/play.hpp"
#include "protocol/primitives.hpp"
#include "protocol/status.hpp"

namespace terila::network {

    void Connection::start() {
        component_ids_ = terila::item::component_ids(data_.component_ids,
            &data_.enchantment_ids);

        asio::error_code ec;
        const auto remote = socket_.remote_endpoint(ec);
        if (!ec) {
            remote_ = remote.address().to_string() + ":" + std::to_string(remote.port());
        }
        do_read();
    }

    void Connection::send(const std::vector<std::uint8_t>& data) {
        if (closed_) {
            return;
        }
        write_queue_.push_back(data);
        if (!writing_) {
            do_write();
        }
    }

    void Connection::send(std::vector<std::uint8_t>&& data) {
        if (closed_) {
            return;
        }
        write_queue_.push_back(std::move(data));
        if (!writing_) {
            do_write();
        }
    }

    void Connection::tick_entities() {
        send_entity_updates(mobs_.mobs());
        send_drop_updates();
        tick_menu();
    }

    const char* Connection::state_name(State s) noexcept {
        switch (s) {
        case State::Handshake:     return "Handshake";
        case State::Status:        return "Status";
        case State::Login:         return "Login";
        case State::Configuration: return "Configuration";
        case State::Play:          return "Play";
        }
        return "Unknown";
    }

    void Connection::process_packets() {
        // One packet at a time, erased before the next is read: the handshake, login and
        // configuration handlers parse the buffer from its start instead of from a framed
        // payload, so a window offset cannot be handed to them.
        while (true) {
            auto packet = terila::protocol::read_packet(read_accum_.data(), read_accum_.size());
            if (!packet.has_value()) {
                return;
            }

            // The raw handlers below read the buffer itself while they run, so the packet
            // is dropped only after it has been handled.
            handle_packet(packet->id, packet->payload);
            read_accum_.erase(read_accum_.begin(),
                read_accum_.begin() + static_cast<std::ptrdiff_t>(packet->consumed));
        }
    }

    void Connection::do_read() {
        auto self = shared_from_this();

        socket_.async_read_some(
            asio::buffer(read_buffer_),
            [self](const asio::error_code& error, std::size_t bytes) {
                self->handle_read(error, bytes);
            }
        );
    }

    void Connection::handle_read(const asio::error_code& error, std::size_t bytes_transferred) {
        if (error == asio::error::eof) {
            LOG_DEBUG(Network, "client disconnected (eof)");
            close();
            return;
        }

        if (error) {
            if (error == asio::error::connection_reset || error == asio::error::connection_aborted) {
                LOG_DEBUG(Network, "client disconnected ({})", error.message());
            }
            else {
                LOG_WARN(Network, "read failed: {} ({})", error.message(), error.value());
            }
            close();
            return;
        }

        if (bytes_transferred == 0) {
            LOG_ERROR(Network, "read 0 bytes without error — TCP invariant violated");
            close();
            return;
        }

        LOG_TRACE(Network, "read {} bytes, accum={}", bytes_transferred, read_accum_.size() + bytes_transferred);

        read_accum_.insert(read_accum_.end(),
            read_buffer_.begin(),
            read_buffer_.begin() + bytes_transferred);

        if (read_accum_.size() > kMaxReadAccum) {
            LOG_ERROR(Network, "read buffer exceeded {} bytes, closing connection", kMaxReadAccum);
            close();
            return;
        }

        process_packets();

        do_read();
    }

    void Connection::handle_packet(std::int32_t id, const std::vector<std::uint8_t>& payload) {
        LOG_TRACE(Network, "packet id=0x{:02X} payload={} bytes in state {}",
            id, payload.size(), state_name(state_));

        switch (state_) {
        case State::Handshake:     handle_handshake(id, payload); break;
        case State::Status:        handle_status(id, payload); break;
        case State::Login:         handle_login(id, payload); break;
        case State::Configuration: handle_configuration(id, payload); break;
        case State::Play:          handle_play(id, payload); break;
        }
    }

    void Connection::close() {
        if (closed_) {
            return;
        }
        closed_ = true;

        if (menu_open_) {
            close_menu();
        }

        if (hub_ && joined_) {
            hub_->leave(entity_id_);
        }

        if (joined_) {
            LOG_INFO(Network, "{} left the game", username_);
        }
        else if (!username_.empty()) {
            LOG_DEBUG(Network, "{}[/{}] disconnected before joining the game", username_, remote_);
        }

        asio::error_code ignored;
        keep_alive_timer_.cancel(ignored);
        socket_.close(ignored);
    }

    void Connection::start_keep_alive() {
        auto self = shared_from_this();
        keep_alive_timer_.expires_after(std::chrono::seconds(15));
        keep_alive_timer_.async_wait([self](const asio::error_code& error) {
            if (error || self->closed_) {
                return;
            }

            std::vector<std::uint8_t> out;
            const std::int64_t id = self->next_keep_alive_id_++;
            if (terila::protocol::write_keep_alive(out, id)) {
                if (!self->keep_alive_answered_ && self->pending_keep_alive_id_ != 0) {
                    // Vanilla drops a client that misses a whole ping interval.
                    LOG_WARN(Network, "keep_alive id={} was never answered, closing the connection",
                        self->pending_keep_alive_id_);
                    self->close();
                    return;
                }
                self->pending_keep_alive_id_ = id;
                self->keep_alive_answered_ = false;
                self->send(out);
                LOG_DEBUG(Network, "keep_alive sent: id={}", id);
            }
            self->start_keep_alive();
        });
    }

    void Connection::do_write() {
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;

        auto self = shared_from_this();
        asio::async_write(
            socket_,
            asio::buffer(write_queue_.front()),
            [self](const asio::error_code& error, std::size_t bytes_transferred) {
                if (error) {
                    LOG_DEBUG(Network, "write failed: {} ({})", error.message(), error.value());
                    self->close();
                    return;
                }

                LOG_TRACE(Network, "wrote {} bytes", bytes_transferred);
                self->write_queue_.pop_front();
                self->do_write();
                self->pump_chunks();
            }
        );
    }

} // namespace terila::network
