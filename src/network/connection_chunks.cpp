#include "network/connection.hpp"

#include "protocol/play.hpp"
#include "world/chunk_encoder.hpp"

#include <algorithm>
#include <cmath>

namespace terila::network {

    std::uint64_t Connection::chunk_key(std::int32_t cx, std::int32_t cz) noexcept {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
            static_cast<std::uint32_t>(cz);
    }

    void Connection::build_chunk_template() {
        if (!chunk_template_.empty()) {
            return;
        }

        const auto resolve_block = [this](std::string_view name) { return block_state(name); };
        const auto resolve_biome = [this](std::string_view name) { return biome_state(name); };

        spec_ = terila::world::flat::FlatSpec{};
        spec_.min_y = data_.overworld.min_y;
        spec_.section_count = data_.overworld.height / 16;

        air_state_ = block_state(spec_.air).value_or(0);
        bedrock_state_ = block_state(spec_.bedrock).value_or(0);
        dirt_state_ = block_state(spec_.dirt).value_or(0);
        grass_state_ = block_state(spec_.grass).value_or(0);

        const auto chunk = terila::world::flat::generate(0, 0, spec_, resolve_block, resolve_biome);
        if (!chunk) {
            LOG_ERROR(Chunk, "cannot generate the flat chunk template");
            return;
        }

        chunk_template_ = terila::world::encode::encode(*chunk);
        LOG_DEBUG(Chunk, "chunk template encoded: {} bytes", chunk_template_.size());
    }

    // A chunk is served from the region file when TRF holds one. Otherwise the flat
    // generator produces it and the encoded payload is persisted, so the world
    // outlives the process instead of being regenerated on every join.
    std::vector<std::uint8_t> Connection::chunk_at(std::int32_t cx, std::int32_t cz) {
        if (store_ != nullptr) {
            std::vector<std::byte> stored;
            const auto loaded = store_->load_chunk(cx, cz, stored);
            if (!loaded) {
                LOG_WARN(Chunk, "trf load ({}, {}) failed: {} ({})", cx, cz,
                    terila::world::trf::to_string(loaded.error().code), loaded.error().detail);
            }
            else if (loaded->present) {
                const auto* begin = reinterpret_cast<const std::uint8_t*>(stored.data());
                return std::vector<std::uint8_t>(begin, begin + stored.size());
            }
        }

        const bool edited = world_.chunk_modified(cx, cz);
        std::vector<std::uint8_t> out;
        if (edited) {
            const auto resolve_block = [this](std::string_view name) { return block_state(name); };
            const auto resolve_biome = [this](std::string_view name) { return biome_state(name); };
            auto chunk = terila::world::flat::generate(cx, cz, spec_, resolve_block, resolve_biome);
            if (chunk) {
                world_.apply(*chunk, air_state_);
                out = terila::world::encode::encode(*chunk);
            }
        }

        if (out.empty()) {
            out = chunk_template_;
            for (int i = 0; i < 4; ++i) {
                out[i] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(cx) >> ((3 - i) * 8)) & 0xFF);
                out[4 + i] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(cz) >> ((3 - i) * 8)) & 0xFF);
            }
        }

        persist_chunk(cx, cz, out, edited);
        return out;
    }

    void Connection::persist_chunk(std::int32_t cx, std::int32_t cz,
        const std::vector<std::uint8_t>& encoded, bool edited) {
        if (store_ == nullptr || encoded.empty()) {
            return;
        }
        const std::uint8_t flags = terila::world::trf::chunk_flags(edited, true, false);
        const auto payload = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(encoded.data()), encoded.size());
        if (auto saved = store_->save_chunk(cx, cz, payload, flags); !saved) {
            LOG_WARN(Chunk, "trf save ({}, {}) failed: {} ({})", cx, cz,
                terila::world::trf::to_string(saved.error().code), saved.error().detail);
        }
    }

    // An edit only exists in BlockWorld until the affected chunk is re-encoded and
    // written back, so every block change persists its chunk immediately.
    void Connection::persist_block_change(const terila::world::BlockPos& target) {
        if (store_ == nullptr) {
            return;
        }

        const std::int32_t cx = target.x >> 4;
        const std::int32_t cz = target.z >> 4;
        const auto resolve_block = [this](std::string_view name) { return block_state(name); };
        const auto resolve_biome = [this](std::string_view name) { return biome_state(name); };
        auto chunk = terila::world::flat::generate(cx, cz, spec_, resolve_block, resolve_biome);
        if (!chunk) {
            return;
        }

        world_.apply(*chunk, air_state_);
        persist_chunk(cx, cz, terila::world::encode::encode(*chunk), true);
    }

    void Connection::queue_chunk_area(std::int32_t center_cx, std::int32_t center_cz) {
        // Rings go outwards, so the ground under the player arrives first.
        for (int ring = 0; ring <= view_distance_; ++ring) {
            for (int dz = -ring; dz <= ring; ++dz) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    if (ring > 0 && std::max(std::abs(dx), std::abs(dz)) != ring) continue;

                    const std::int32_t cx = center_cx + dx;
                    const std::int32_t cz = center_cz + dz;
                    if (!sent_chunks_.insert(chunk_key(cx, cz)).second) continue;
                    pending_chunks_.push_back(ChunkPos{ cx, cz });
                }
            }
        }

        LOG_DEBUG(Chunk, "chunks queued around ({}, {}): {} waiting", center_cx, center_cz,
            pending_chunks_.size());
        pump_chunks();
    }

    // Chunks left far behind are forgotten so they are streamed again on the way back.
    void Connection::forget_far_chunks(std::int32_t center_cx, std::int32_t center_cz) {
        const int limit = view_distance_ + 2;
        for (auto it = sent_chunks_.begin(); it != sent_chunks_.end();) {
            const auto cx = static_cast<std::int32_t>(*it >> 32);
            const auto cz = static_cast<std::int32_t>(*it & 0xFFFFFFFFu);
            if (std::abs(cx - center_cx) > limit || std::abs(cz - center_cz) > limit) {
                it = sent_chunks_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void Connection::pump_chunks() {
        if (pending_chunks_.empty() || write_queue_.size() >= kMaxWriteQueue) {
            return;
        }

        std::vector<std::uint8_t> out;
        if (!terila::protocol::write_chunk_batch_start(out)) return;
        send(out);
        out.clear();

        int count = 0;
        while (!pending_chunks_.empty() && count < kChunksPerBatch) {
            const auto pos = pending_chunks_.front();
            pending_chunks_.pop_front();

            if (!terila::protocol::write_chunk_data(out, chunk_at(pos.x, pos.z))) {
                return;
            }
            send(out);
            out.clear();
            ++count;
        }

        if (!terila::protocol::write_chunk_batch_finished(out, count)) return;
        send(out);

        LOG_DEBUG(Chunk, "chunk batch sent: {} chunks, {} waiting", count, pending_chunks_.size());
    }

    void Connection::update_player_chunk(double x, double z) {
        const auto cx = static_cast<std::int32_t>(std::floor(x / 16.0));
        const auto cz = static_cast<std::int32_t>(std::floor(z / 16.0));
        if (cx == center_cx_ && cz == center_cz_) {
            return;
        }

        center_cx_ = cx;
        center_cz_ = cz;
        LOG_TRACE(Network, "chunk center moved to ({}, {})", cx, cz);

        std::vector<std::uint8_t> out;
        if (terila::protocol::write_set_center_chunk(out, cx, cz)) {
            send(out);
        }

        forget_far_chunks(cx, cz);
        queue_chunk_area(cx, cz);
    }

} // namespace terila::network
