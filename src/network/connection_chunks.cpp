#include "network/connection.hpp"

#include "protocol/play.hpp"
#include "protocol/player_state.hpp"
#include "world/chunk_encoder.hpp"
#include "world/chunk_decoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace terila::network {

    std::uint64_t Connection::chunk_key(std::int32_t cx, std::int32_t cz) noexcept {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
            static_cast<std::uint32_t>(cz);
    }

    void Connection::init_terrain() {
        if (terrain_ready_) {
            return;
        }

        // The overlay needs the air id even when another connection already prepared the
        // shared generator.
        air_state_ = block_state("minecraft:air").value_or(0);

        // Every dimension owns its generator: the nether's height, sea level and fluid are
        // not the overworld's, and a player who steps back out must find the overworld he
        // left instead of a copy rebuilt over the nether's settings.
        terila::world::WorldGenerator& generator =
            terila::world::WorldContext::shared().generator(dimension_);
        terrain_ = &generator;
        if (generator.ready()) {
            sea_level_ = generator.sea_level();
            terrain_ready_ = true;
            return;
        }

        // The generator outlives every connection and its chunk workers keep calling these
        // while other players stream, so they hold the registry itself. Capturing the
        // connection would leave the generator reading a destroyed object the moment the
        // player who happened to initialise the dimension logs out.
        const terila::data::RegistryData* registry = &data_;
        const auto resolve_block = [registry](std::string_view name)
            -> std::optional<std::int32_t> {
            const auto found = registry->block_state_ids.find(std::string(name));
            if (found == registry->block_state_ids.end()) {
                return std::nullopt;
            }
            return found->second;
        };
        const auto resolve_biome = [registry](std::string_view name)
            -> std::optional<std::int32_t> {
            const auto found = registry->biome_ids.find(std::string(name));
            if (found == registry->biome_ids.end()) {
                return std::nullopt;
            }
            return found->second;
        };
        const auto resolve_state_text = [registry](std::string_view text) {
            return world::worldgen::template_state_id(text, registry->block_states,
                registry->block_state_ids);
        };
        const auto rotate_state = world::worldgen::template_rotator(data_);

        // A flat world is a shape the overworld generator builds; the nether and the end
        // always come from their own noise settings, whatever level-type says.
        const auto world = dimension_ == terila::world::DimensionKind::Overworld
            ? world_type_ : terila::world::WorldType::Terrain;
        const auto dimension_type = terila::world::worldgen::dimension_type_of(dimension_);

        if (!generator.init(world, dimension_type.min_y, dimension_type.height / 16,
                world_seed_, resolve_block, resolve_biome, resolve_state_text, dimension_,
                rotate_state)) {
            LOG_CRITICAL(Chunk, "cannot initialise the {} world generator — no player can join "
                "until the block and biome registries are fixed",
                terila::world::worldgen::dimension_name(dimension_));
            return;
        }
        sea_level_ = generator.sea_level();
        terrain_ready_ = true;

        LOG_INFO(Chunk, "{} ({}) ready: {} sections from y={}, sea level {}, seed {}",
            terila::world::worldgen::dimension_name(dimension_),
            terila::world::world_type_name(generator.type()), generator.section_count(),
            generator.min_y(), generator.sea_level(), generator.seed());
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
        return chunk_at(cx, cz, std::nullopt);
    }

    // `prepared` is a column the worker pool generated for this very coordinate. Only the
    // paths that need a generated chunk consume it, so a cache or region hit never pays for
    // a generation it would throw away.
    std::vector<std::uint8_t> Connection::chunk_at(std::int32_t cx, std::int32_t cz,
        std::optional<terila::world::PreparedChunk> prepared) {
        init_terrain();

        const auto take = [&]() {
            if (prepared) {
                auto taken = std::move(*prepared);
                prepared.reset();
                return taken;
            }
            terila::world::PreparedChunk fresh;
            fresh.chunk = terrain_->generate(cx, cz);
            return fresh;
        };

        auto& cache = terila::world::WorldContext::shared().chunks(dimension_);

        // A save still waiting its turn is the newest copy of this column. Loading the
        // region instead would hand back the version from before the last edit, and those
        // old blocks would then be recorded as edits of their own.
        for (const auto& save : pending_saves_) {
            if (save.x == cx && save.z == cz && save.dimension == dimension_) {
                return save.bytes;
            }
        }

        // The cache and the store already hold payloads with their light computed: every
        // path that edits a column relights it before saving, and an edit drops the cached
        // copy. Serving them first is what keeps a chunk load from re-running the light
        // engine (and regenerating its eight neighbours) for a column that was already lit.
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
                auto decoded = terila::world::decode::decode(
                    reinterpret_cast<const std::uint8_t*>(stored.data()),
                    stored.size(), terrain_->section_count());
                if (decoded) {
                    // The baseline is the generated chunk the overlay is diffed against;
                    // it is needed again below for the cache, so it is generated once.
                    auto taken = take();
                    auto generated = std::move(taken.chunk);
                    apply_loaded_chunk(*decoded, generated);

                    // A save made before the light rules knew about placed light sources
                    // carries light from the old rules, so a lamp in it streams back dark.
                    // Lighting the column again once rewrites it with the flag set: the cost
                    // is one relight per edited column, not one per visit.
                    if (!loaded->light_full && loaded->edited) {
                        relight_chunk(*decoded);
                        auto out = terila::world::encode::encode(*decoded);
                        cache.put(std::move(generated), out);
                        queue_persist(cx, cz, out, true);
                        LOG_DEBUG(Chunk,
                            "chunk ({}, {}) relit from a save older than the light rules",
                            cx, cz);
                        return out;
                    }

                    // The stored slot was written after an edit, so its light already
                    // matches its blocks; it streams back verbatim either way.
                    auto out = std::vector<std::uint8_t>(
                        reinterpret_cast<const std::uint8_t*>(stored.data()),
                        reinterpret_cast<const std::uint8_t*>(stored.data()) + stored.size());
                    cache.put(std::move(generated), out);
                    return out;
                }
                LOG_WARN(Chunk, "trf decode ({}, {}) failed, regenerating", cx, cz);
            }
        }

        auto taken = take();
        auto chunk = std::move(taken.chunk);

        // No stored copy exists, so this column may carry edits that were never written:
        // applying and relighting here is what keeps them visible. A plain column is
        // deterministic, so it is cached rather than written back on every visit.
        if (world_.chunk_modified(cx, cz, dimension_)) {
            world_.apply(chunk, air_state_, dimension_);
            relight_chunk(chunk);
            auto out = terila::world::encode::encode(chunk);
            cache.put(std::move(chunk), out);
            queue_persist(cx, cz, out, true);
            return out;
        }

        auto out = taken.encoded.empty() ? terila::world::encode::encode(chunk)
                                         : std::move(taken.encoded);
        cache.put(std::move(chunk), out);
        return out;
    }

    // A freshly generated chunk is deterministic, so it is safe to write it back after
    // it has been streamed: the tick drains this queue instead of the send path.
    void Connection::queue_persist(std::int32_t cx, std::int32_t cz,
        const std::vector<std::uint8_t>& encoded, bool edited) {
        if (store_ == nullptr || encoded.empty()) {
            return;
        }
        // A save queue that cannot keep up with exploration would hold the whole world in
        // memory; the newest copy of a column is the one worth writing, so the oldest
        // entries give way.
        while (pending_saves_.size() >= kMaxPendingSaves) {
            pending_saves_.pop_front();
        }
        pending_saves_.push_back(PendingSave{ cx, cz, encoded, edited, dimension_ });
    }

    void Connection::flush_pending_saves(std::size_t budget) {
        for (std::size_t i = 0; i < budget && !pending_saves_.empty(); ++i) {
            auto save = std::move(pending_saves_.front());
            pending_saves_.pop_front();
            persist_chunk(save.x, save.z, save.bytes, save.edited, save.dimension);
        }
    }

    void Connection::persist_chunk(std::int32_t cx, std::int32_t cz,
        const std::vector<std::uint8_t>& encoded, bool edited,
        terila::world::DimensionKind kind) {
        auto* store = stores_[static_cast<std::size_t>(terila::world::dimension_index(kind))];
        if (store == nullptr || encoded.empty()) {
            return;
        }
        // Everything written from here on carries light computed by the current rules, which
        // is what lets the loader stream it back without lighting the column again.
        const std::uint8_t flags = terila::world::trf::chunk_flags(edited, true, true);
        const auto payload = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(encoded.data()), encoded.size());
        if (auto saved = store->save_chunk(cx, cz, payload, flags); !saved) {
            LOG_CRITICAL(Chunk, "trf save ({}, {}) failed: {} ({}) — edited chunks now live only "
                "in memory, a restart loses them", cx, cz,
                terila::world::trf::to_string(saved.error().code), saved.error().detail);
        }
    }

    // The generator lit the world before any block was touched. Encoding an edited chunk as
    // it is therefore carries the old light: a block that was dug out keeps the darkness it
    // had while it was solid, which reads as a black hole in the terrain. Re-running the
    // light engine over the overlay puts the sun back into every hole and shadow under every
    // block the player placed.
    void Connection::relight_chunk(terila::world::Chunk& chunk) {
        const auto& rules = terrain_->light_rules();
        const int min_y = terrain_->min_y();
        const int world_top = min_y + terrain_->section_count() * 16 - 1;

        // The engine reads 15 blocks past the border, so the neighbouring columns are
        // generated once and sampled with the overlay on top of them.
        std::unordered_map<std::uint64_t, terila::world::Chunk> neighbours;
        const auto chunk_for = [&](std::int32_t cx, std::int32_t cz) -> const terila::world::Chunk& {
            if (cx == chunk.x && cz == chunk.z) {
                return chunk;
            }
            const auto key = terila::world::BlockWorld::chunk_key(cx, cz);
            const auto found = neighbours.find(key);
            if (found != neighbours.end()) {
                return found->second;
            }
            // The generator cache usually holds the columns that were just streamed, so a
            // relight does not have to regenerate up to eight neighbours.
            if (const auto* cached =
                terila::world::WorldContext::shared().chunks(dimension_).find(cx, cz)) {
                return neighbours.emplace(key, cached->chunk).first->second;
            }
            return neighbours.emplace(key, terrain_->generate(cx, cz)).first->second;
        };

        const auto state_at_edited = [&](std::int32_t x, std::int32_t y,
            std::int32_t z) -> std::int32_t {
            const std::int32_t cx = chunk_coordinate(x);
            const std::int32_t cz = chunk_coordinate(z);
            const auto& c = chunk_for(cx, cz);
            const int local_y = y - c.min_y;
            if (local_y < 0 || local_y >= c.section_count * 16) {
                return air_state_;
            }
            const auto section = static_cast<std::size_t>(local_y / 16);
            const auto index = ((local_y % 16) * 16 + (z & 15)) * 16 + (x & 15);
            const std::int32_t generated =
                terila::world::section_state(c.sections[section], index);
            if (!world_.chunk_modified(cx, cz, dimension_)) {
                return generated;
            }
            return world_.get(terila::world::BlockPos{ x, y, z }, generated, dimension_);
        };

        const auto sky_top = [&](std::int32_t x, std::int32_t z) {
            const auto generated_top = [&](std::int32_t gx, std::int32_t gz) {
                const auto& c = chunk_for(chunk_coordinate(gx), chunk_coordinate(gz));
                for (int y = world_top; y >= c.min_y; --y) {
                    const int local_y = y - c.min_y;
                    const auto& sec = c.sections[static_cast<std::size_t>(local_y / 16)];
                    const auto index = ((local_y % 16) * 16 + (gz & 15)) * 16 + (gx & 15);
                    if (!rules.of(terila::world::section_state(sec, index)).sky_down) {
                        return y;
                    }
                }
                return c.min_y - 1;
            };

            return terila::world::edited_sky_top(min_y, world_top, x, z, generated_top,
                state_at_edited, [&rules](std::int32_t state) { return rules.of(state).sky_down; });
        };

        terila::world::LightEngine::compute(chunk, rules, sky_top, state_at_edited);
        LOG_TRACE(Chunk, "relit chunk ({}, {}) with {} edits", chunk.x, chunk.z,
            world_.modified_count(chunk.x, chunk.z, dimension_));
    }

    // An edit only exists in BlockWorld until the affected chunk is re-encoded and
    // written back, so every block change persists its chunk immediately.
    void Connection::persist_block_change(const terila::world::BlockPos& target) {
        persist_chunk_edits(chunk_coordinate(target.x), chunk_coordinate(target.z));
    }

    // The same save, addressed by column: the block simulation moves several cells of one
    // column in a tick, and re-encoding it once per cascade is what keeps that affordable.
    void Connection::persist_chunk_edits(std::int32_t cx, std::int32_t cz) {
        // The column changed, so the shared copy no longer matches what this player holds.
        terila::world::WorldContext::shared().chunks(dimension_).erase(cx, cz);
        if (store_ == nullptr) {
            return;
        }

        init_terrain();
        auto chunk = terrain_->generate(cx, cz);
        world_.apply(chunk, air_state_, dimension_);
        relight_chunk(chunk);
        LOG_DEBUG(Chunk, "queued chunk ({}, {}) with {} edits total", cx, cz,
            world_.modified_count(cx, cz, dimension_));
        // Queued instead of written here: a region write inside the click handler stalls
        // every other player on the io thread.
        queue_persist(cx, cz, terila::world::encode::encode(chunk), true);
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
        // The workers generate on the shared generator, so it has to exist before the first
        // batch is handed out: init() is idempotent and this is the only caller that runs it
        // off the chunk path.
        init_terrain();

        // A dimension change bumps the epoch: the batch below was generated for the world
        // the player has already left, so it is dropped instead of streamed over the new one.
        const std::uint32_t epoch = chunk_epoch_;

        while (!pending_chunks_.empty()) {
            if (epoch != chunk_epoch_) {
                co_return;
            }
            if (write_queue_.size() >= kMaxWriteQueue) {
                co_await terila::async::Yield{};
                continue;
            }

            // Generation and encoding are the expensive half of streaming and both are pure,
            // so a whole batch is handed to the workers at once. The frames below are still
            // produced in queue order, which keeps the ring order the client sees, and a
            // chunk that is not ready yet yields instead of stalling the io thread that
            // every other player's packets arrive on.
            std::vector<ChunkPos> batch;
            batch.reserve(static_cast<std::size_t>(kChunksPerBatch));
            while (!pending_chunks_.empty() &&
                batch.size() < static_cast<std::size_t>(kChunksPerBatch)) {
                batch.push_back(pending_chunks_.front());
                pending_chunks_.pop_front();
            }

            auto& jobs = terila::world::ChunkJobs::shared();
            const auto wake = chunk_completion();
            std::vector<std::future<terila::world::PreparedChunk>> prepared;
            prepared.reserve(batch.size());
            for (const auto& pos : batch) {
                prepared.push_back(jobs.prepare(dimension_, pos.x, pos.z, wake));
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
            for (std::size_t i = 0; i < batch.size(); ++i) {
                while (prepared[i].wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready) {
                    co_await terila::async::Yield{};
                }
                if (epoch != chunk_epoch_) {
                    co_return;
                }

                auto payload = chunk_at(batch[i].x, batch[i].z, prepared[i].get());
                if (!terila::protocol::write_chunk_data(group, payload)) {
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

    // A worker that finishes posts this back to the connection's executor: the stream is
    // resumed immediately instead of waiting for the next tick or the next write completion
    // to poll the future it is parked on.
    std::function<void()> Connection::chunk_completion() {
        auto executor = socket_.get_executor();
        const std::weak_ptr<Connection> weak = weak_from_this();
        return [executor = std::move(executor), weak]() mutable {
            asio::post(executor, [weak] {
                if (const auto self = weak.lock()) {
                    self->pump_chunks();
                }
            });
        };
    }

    void Connection::report_chunk_stream_failure() {
        try {
            chunk_stream_.rethrow_if_failed();
        }
        catch (const std::exception& error) {
            LOG_ERROR(Chunk, "chunk stream stopped: {}", error.what());
        }
        catch (...) {
            LOG_ERROR(Chunk, "chunk stream stopped with an unknown failure");
        }
    }

    void Connection::pump_chunks() {
        // A running stream owns its batch: the chunks it already took out of the queue are
        // not visible in pending_chunks_, so resuming it must not depend on the queue. This
        // is what used to leave the last batch of a view distance half sent, with the
        // client showing an endless "Loading terrain".
        if (!chunk_stream_.finished()) {
            chunk_stream_.resume();
            if (chunk_stream_.finished()) {
                report_chunk_stream_failure();
            }
            return;
        }
        // Nothing queued: do not build a coroutine frame only to have it return at once,
        // because this runs after every completed write.
        if (pending_chunks_.empty()) {
            return;
        }
        chunk_stream_.restart(stream_chunks());
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

    // Stepping into another dimension replaces the whole world the client holds. The move
    // reopens the region files of that dimension, drops every column the old one streamed and
    // starts the view distance again from the new generator; an unfinished batch of the old
    // world is left to the epoch guard in stream_chunks.
    bool Connection::enter_dimension(terila::world::DimensionKind kind, double x, double y,
        double z, float yaw, float pitch) {
        using terila::world::dimension_index;

        if (state_ != State::Play || closed_ || kind == dimension_) {
            return false;
        }

        const auto previous = dimension_;
        dimension_ = kind;
        store_ = stores_[static_cast<std::size_t>(dimension_index(kind))];
        terrain_ = &terila::world::WorldContext::shared().generator(kind);
        // The new dimension resolves its own ids and sea level; the old world's are not
        // valid here.
        terrain_ready_ = false;
        init_terrain();

        if (terrain_ == nullptr || !terrain_->ready()) {
            // Nothing to stand on: the player stays where he is rather than dropping into an
            // empty world.
            dimension_ = previous;
            store_ = stores_[static_cast<std::size_t>(dimension_index(previous))];
            terrain_ = &terila::world::WorldContext::shared().generator(previous);
            terrain_ready_ = false;
            init_terrain();
            return false;
        }

        ++chunk_epoch_;
        pending_chunks_.clear();
        sent_chunks_.clear();
        known_entities_.clear();
        known_drops_.clear();
        abort_breaking();

        player_x_ = x;
        player_y_ = y;
        player_z_ = z;
        yaw_ = yaw;
        pitch_ = pitch;

        std::vector<std::uint8_t> out;
        const auto name = terila::world::worldgen::dimension_name(kind);
        if (!terila::protocol::write_respawn(out, name, static_cast<std::int64_t>(world_seed_),
                static_cast<std::uint8_t>(gamemode_), sea_level_,
                terila::world::worldgen::dimension_type_id(kind))) {
            return false;
        }
        send(out);

        // Every respawn has to be followed by "start waiting for chunks". Without it the
        // client keeps its "Loading terrain" screen up until its own thirty second timeout,
        // even though the new world is already streaming.
        out.clear();
        if (terila::protocol::write_game_event(out, 13, 0.0f)) {
            send(out);
        }
        send_health();

        out.clear();
        if (terila::protocol::write_player_position(out, next_teleport_id_++, x, y, z,
                0.0, 0.0, 0.0, yaw, pitch)) {
            send(out);
        }

        if (hub_) {
            hub_->move(entity_id_, PlayerHub::Position{ x, y, z, yaw, pitch, kind });
        }

        // The client threw its world away with the respawn packet, so every column is
        // streamed again even though the centre may not have moved in chunk coordinates.
        center_cx_ = chunk_coordinate(x);
        center_cz_ = chunk_coordinate(z);
        out.clear();
        if (terila::protocol::write_set_center_chunk(out, center_cx_, center_cz_)) {
            send(out);
        }
        queue_chunk_area(center_cx_, center_cz_);

        LOG_INFO(Network, "{} entered {} at ({:.1f}, {:.1f}, {:.1f})", username_, name, x, y, z);
        return true;
    }

} // namespace terila::network
