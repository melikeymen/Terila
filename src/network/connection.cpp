#include "network/connection.hpp"

#include "protocol/disconnect.hpp"
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

        // The command grammar is built once per connection: it carries the item and
        // component registries, and the tree sent on login is generated from it.
        registry_ = std::make_unique<commands::Registry>(data_, component_ids_);
        registry_->install_all();

        asio::error_code ec;
        // Nagle would hold every small packet back for the peer's delayed ack, which shows
        // up as ten millisecond stalls between writes and made a whole view distance crawl.
        socket_.set_option(asio::ip::tcp::no_delay(true), ec);
        if (ec) {
            LOG_DEBUG(Network, "cannot disable Nagle: {}", ec.message());
        }

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
        // A held-down survival dig counts down once per tick.
        tick_breaking();
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
        while (!closed_ && !closing_) {
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
        // The goodbye is already queued: whatever the peer still sends is not acted on.
        if (closing_) {
            return;
        }

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

    void Connection::kick(std::string_view reason) {
        LOG_INFO(Network, "{}[/{}] kicked: {}", username_, remote_, reason);
        disconnect_with(reason);
    }

    void Connection::disconnect_with(std::string_view reason) {
        if (closed_ || closing_) {
            return;
        }

        const auto& ids = terila::protocol::packet_ids();
        std::vector<std::uint8_t> out;
        bool queued = false;
        switch (state_) {
        case State::Login:
            queued = terila::protocol::write_login_disconnect(out, reason);
            break;
        case State::Configuration:
            queued = terila::protocol::write_component_disconnect(out, ids.config_disconnect, reason);
            break;
        case State::Play:
            queued = terila::protocol::write_component_disconnect(out, ids.play_disconnect, reason);
            break;
        case State::Handshake:
        case State::Status:
            // Neither state gives the client a packet to show a reason in.
            break;
        }

        LOG_INFO(Network, "{}[/{}] disconnected: {}",
            username_.empty() ? "unknown" : username_, remote_, reason);

        if (!queued) {
            close();
            return;
        }

        // The packet has to leave before the socket does, so close() runs from the write
        // that empties the queue instead of here.
        closing_ = true;
        asio::error_code ignored;
        keep_alive_timer_.cancel(ignored);
        login_timer_.cancel(ignored);
        hang_up_after_write_ = true;
        send(out);
        if (!writing_) {
            hang_up_after_write_ = false;
            close();
        }
    }

    void Connection::start_login_timeout() {
        auto self = shared_from_this();
        login_timer_.expires_after(std::chrono::seconds(30));
        login_timer_.async_wait([self](const asio::error_code& error) {
            if (error || self->closed_ || self->closing_) {
                return;
            }
            if (self->state_ == State::Play) {
                return;
            }
            LOG_WARN(Network, "login from {} did not finish in time, closing it", self->remote_);
            self->disconnect_with("Took too long to log in");
        });
    }

    void Connection::close() {
        if (closed_) {
            return;
        }
        closed_ = true;
        // Whatever is still queued for this player is never streamed, so the workers stop
        // before generating it.
        chunk_work_alive_->store(false, std::memory_order_relaxed);

        if (menu_open_) {
            close_menu();
        }

        // Edits made since the last flush live only in this queue, so writing it here is
        // what stops a relog from restoring blocks the player had already broken.
        flush_pending_saves(pending_saves_.size());

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
        login_timer_.cancel(ignored);
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
            // The disconnect packet was the last thing queued, so this is where the socket
            // finally goes.
            if (hang_up_after_write_) {
                hang_up_after_write_ = false;
                close();
            }
            return;
        }
        writing_ = true;

        auto self = shared_from_this();

        // The socket takes the first few queued packets as one gathered write, so a burst of
        // small packets costs one call instead of one per packet. The buffers point into the
        // queue, which keeps its elements valid while it is appended to.
        std::vector<asio::const_buffer> buffers;
        std::size_t taken = 0;
        while (taken < write_queue_.size() && taken < kMaxWriteQueue) {
            const std::vector<std::uint8_t>& queued = write_queue_[taken];
            ++taken;
            if (!queued.empty()) {
                buffers.emplace_back(queued.data(), queued.size());
            }
        }
        if (buffers.empty()) {
            // Nothing but empty buffers: drop them and look at what follows.
            write_queue_.erase(write_queue_.begin(),
                write_queue_.begin() + static_cast<std::ptrdiff_t>(taken));
            writing_ = false;
            do_write();
            return;
        }

        // The count is captured as a value: moving the list into the handler would race the
        // argument that hands the same list to the write.
        const std::size_t buffer_count = buffers.size();
        asio::async_write(
            socket_,
            buffers,
            [self, taken, buffer_count](
                const asio::error_code& error, std::size_t bytes_transferred) {
                if (error) {
                    LOG_DEBUG(Network, "write failed: {} ({})", error.message(), error.value());
                    self->close();
                    return;
                }

                LOG_TRACE(Network, "wrote {} bytes in {} buffers", bytes_transferred, buffer_count);
                self->write_queue_.erase(self->write_queue_.begin(),
                    self->write_queue_.begin() + static_cast<std::ptrdiff_t>(taken));
                self->do_write();
                self->pump_chunks();
            }
        );
    }

} // namespace terila::network
