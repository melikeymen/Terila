#include "network/connection.hpp"

#include "protocol/play.hpp"
#include "world/chunk_encoder.hpp"
#include "world/chunk_decoder.hpp"

#include <algorithm>
#include <cmath>

namespace terila::network {

    std::uint64_t Connection::chunk_key(std::int32_t cx, std::int32_t cz) noexcept {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
            static_cast<std::uint32_t>(cz);
    }

    void Connection::init_terrain() {
        // The overlay needs the air id even when another connection already prepared the
        // shared generator.
        air_state_ = block_state("minecraft:air").value_or(0);

        if (terrain_.ready()) {
            return;
        }

        const auto resolve_block = [this](std::string_view name) { return block_state(name); };
        const auto resolve_biome = [this](std::string_view name) { return biome_state(name); };

        if (!terrain_.init(world_type_, data_.overworld.min_y, data_.overworld.height / 16,
                world_seed_, resolve_block, resolve_biome)) {
            LOG_CRITICAL(Chunk, "cannot initialise the {} world generator — no player can join "
                "until the block and biome registries are fixed",
                terila::world::world_type_name(world_type_));
            return;
        }

        LOG_INFO(Chunk, "{} world ready: {} sections from y={}, sea level {}, seed {}",
            terila::world::world_type_name(terrain_.type()), terrain_.section_count(),
            terrain_.min_y(), terrain_.sea_level(), terrain_.seed());
    }

    // The overlay only holds blocks that differ from the generated terrain, so a stored
    // chunk has to be compared against generation. Generating the chunk once and
    // comparing the two section arrays is what makes that cheap: state_at resolves a
    // whole column (plus its tree neighbourhood) for every single block, roughly 700x
    // more work than one generate() call for the same chunk.
    void Connection::apply_loaded_chunk(const terila::world::Chunk& chunk,
        const terila::world::Chunk& generated) {
        const int sections = std::min(static_cast<int>(chunk.sections.size()),
            static_cast<int>(generated.sections.size()));

        for (int sy = 0; sy < sections; ++sy) {
            const auto& sec = chunk.sections[static_cast<std::size_t>(sy)];
            const auto& base = generated.sections[static_cast<std::size_t>(sy)];
            const std::int32_t section_y = chunk.min_y + sy * 16;
            for (int i = 0; i < 4096; ++i) {
                const std::int32_t state = terila::world::section_state(sec, i);
                if (state == terila::world::section_state(base, i)) {
                    continue;
                }
                const std::int32_t y = section_y + (i / 256);
                const std::int32_t z = chunk.z * 16 + ((i / 16) % 16);
                const std::int32_t x = chunk.x * 16 + (i % 16);
                world_.set(terila::world::BlockPos{ x, y, z }, state);
            }
        }
    }

    // A chunk is served from the region file when TRF holds one. Otherwise the flat
    // generator produces it and the encoded payload is persisted, so the world
    // outlives the process instead of being regenerated on every join.
    //
    // Edits made in this session win over the region file: the overlay is the truth while
    // the server runs, so re-streaming a chunk can never resurrect an older disk copy and
    // drop blocks the player has placed since.
    std::vector<std::uint8_t> Connection::chunk_at(std::int32_t cx, std::int32_t cz) {
        init_terrain();

        auto& cache = terila::world::WorldContext::shared().chunks();

        if (world_.chunk_modified(cx, cz)) {
            auto chunk = terrain_.generate(cx, cz);
            world_.apply(chunk, air_state_);
            auto out = terila::world::encode::encode(chunk);
            LOG_DEBUG(Chunk, "chunk ({}, {}) served from {} in-memory edits", cx, cz,
                world_.modified_count(cx, cz));
            queue_persist(cx, cz, out);
            return out;
        }

        // Nobody has edited this column, so the cached copy is what a fresh generation
        // would produce and it can go straight out.
        if (const auto* cached = cache.find(cx, cz); cached != nullptr) {
            return cached->encoded;
        }

        if (store_ != nullptr) {
            std::vector<std::byte> stored;
            const auto loaded = store_->load_chunk(cx, cz, stored);
            if (!loaded) {
                // A missing or damaged chunk is local; an I/O error means the whole region
                // is unreachable, which is persistence gone rather than one chunk.
                if (loaded.error().code == terila::world::trf::TrfErrorCode::Io) {
                    LOG_CRITICAL(Chunk, "trf load ({}, {}) hit an I/O error: {} ({})", cx, cz,
                        terila::world::trf::to_string(loaded.error().code),
                        loaded.error().detail);
                }
                else if (loaded.error().code != terila::world::trf::TrfErrorCode::NotFound) {
                    LOG_WARN(Chunk, "trf load ({}, {}) failed: {} ({})", cx, cz,
                        terila::world::trf::to_string(loaded.error().code),
                        loaded.error().detail);
                }
            }
            else if (loaded->present) {
                const auto decoded = terila::world::decode::decode(
                    reinterpret_cast<const std::uint8_t*>(stored.data()),
                    stored.size(), terrain_.section_count());
                if (decoded) {
                    // The baseline is the generated chunk the overlay is diffed against;
                    // it is needed again below for the cache, so it is generated once.
                    auto generated = terrain_.generate(cx, cz);
                    apply_loaded_chunk(*decoded, generated);
                    // The stored slot already holds the wire-ready payload, so stream it
                    // back verbatim instead of decoding and re-encoding the same chunk.
                    auto out = std::vector<std::uint8_t>(
                        reinterpret_cast<const std::uint8_t*>(stored.data()),
                        reinterpret_cast<const std::uint8_t*>(stored.data()) + stored.size());
                    cache.put(std::move(generated), out);
                    return out;
                }
                LOG_WARN(Chunk, "trf decode ({}, {}) failed, regenerating", cx, cz);
            }
        }

        auto chunk = terrain_.generate(cx, cz);
        auto out = terila::world::encode::encode(chunk);
        cache.put(chunk, out);
        queue_persist(cx, cz, out);
        return out;
    }

    // A freshly generated chunk is deterministic, so it is safe to write it back after
    // it has been streamed: the tick drains this queue instead of the send path.
    void Connection::queue_persist(std::int32_t cx, std::int32_t cz,
        const std::vector<std::uint8_t>& encoded) {
        if (store_ == nullptr || encoded.empty()) {
            return;
        }
        // A save queue that cannot keep up with exploration would hold the whole world in
        // memory; the newest copy of a column is the one worth writing, so the oldest
        // entries give way.
        while (pending_saves_.size() >= kMaxPendingSaves) {
            pending_saves_.pop_front();
        }
        pending_saves_.push_back(PendingSave{ cx, cz, encoded });
    }

    void Connection::flush_pending_saves(std::size_t budget) {
        for (std::size_t i = 0; i < budget && !pending_saves_.empty(); ++i) {
            auto save = std::move(pending_saves_.front());
            pending_saves_.pop_front();
            persist_chunk(save.x, save.z, save.bytes, false);
        }
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
            LOG_CRITICAL(Chunk, "trf save ({}, {}) failed: {} ({}) — edited chunks now live only "
                "in memory, a restart loses them", cx, cz,
                terila::world::trf::to_string(saved.error().code), saved.error().detail);
        }
    }

    // An edit only exists in BlockWorld until the affected chunk is re-encoded and
    // written back, so every block change persists its chunk immediately.
    void Connection::persist_block_change(const terila::world::BlockPos& target) {
        const std::int32_t cx = chunk_coordinate(target.x);
        const std::int32_t cz = chunk_coordinate(target.z);

        // The column changed, so the shared copy no longer matches what this player holds.
        terila::world::WorldContext::shared().chunks().erase(cx, cz);
        if (store_ == nullptr) {
            return;
        }

        init_terrain();
        auto chunk = terrain_.generate(cx, cz);
        world_.apply(chunk, air_state_);
        LOG_DEBUG(Chunk, "queued chunk ({}, {}) with {} edits total", cx, cz,
            world_.modified_count(cx, cz));
        // Queued instead of written here: a region write inside the click handler stalls
        // every other player on the io thread.
        queue_persist(cx, cz, terila::world::encode::encode(chunk));
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

    // Streaming a chunk area is a coroutine. The batch it is in the middle of is its own
    // local state, so when the socket falls behind the stream pauses instead of tearing a
    // half sent batch down, and the next tick picks up exactly where it stopped.
    terila::async::Task<void> Connection::stream_chunks() {
        while (!pending_chunks_.empty()) {
            if (write_queue_.size() >= kMaxWriteQueue) {
                co_await terila::async::Yield{};
                continue;
            }

            std::vector<std::uint8_t> out;
            if (!terila::protocol::write_chunk_batch_start(out)) {
                co_return;
            }
            send(std::move(out));

            // Several chunk packets share one write: each one is already framed, so they
            // can sit in the same buffer and cost a single syscall instead of one per
            // chunk.
            std::vector<std::uint8_t> group;
            int count = 0;
            while (!pending_chunks_.empty() && count < kChunksPerBatch) {
                const auto pos = pending_chunks_.front();
                pending_chunks_.pop_front();

                if (!terila::protocol::write_chunk_data(group, chunk_at(pos.x, pos.z))) {
                    co_return;
                }
                ++count;

                if (group.size() >= kChunkGroupBytes || write_queue_.size() >= kMaxWriteQueue) {
                    send(std::move(group));
                    group.clear();
                }

                // A busy socket is the moment to hand the frame back and let the write
                // completion drive the rest.
                if (write_queue_.size() >= kMaxWriteQueue) {
                    co_await terila::async::Yield{};
                }
            }
            if (!group.empty()) {
                send(std::move(group));
            }

            std::vector<std::uint8_t> finished;
            if (!terila::protocol::write_chunk_batch_finished(finished, count)) {
                co_return;
            }
            send(std::move(finished));

            LOG_DEBUG(Chunk, "chunk batch sent: {} chunks, {} waiting", count,
                pending_chunks_.size());
            co_await terila::async::Yield{};
        }
    }

    void Connection::pump_chunks() {
        // Nothing queued: do not build a coroutine frame only to have it return at once,
        // because this runs after every completed write.
        if (pending_chunks_.empty()) {
            return;
        }
        if (chunk_stream_.finished()) {
            chunk_stream_.restart(stream_chunks());
        }
        chunk_stream_.resume();
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
