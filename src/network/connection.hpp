#pragma once

#include <asio.hpp>

#include "data/registry_data.hpp"
#include "entity/mob.hpp"
#include "log/logger.hpp"
#include "network/player_hub.hpp"
#include "protocol/block.hpp"
#include "world/block_world.hpp"
#include "world/flat_generator.hpp"
#include "world/trf/trf_store.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace terila::network {

    // One TCP peer. The class only declares the state machine: framing and the handshake,
    // status, login, configuration and play handlers live in their own translation units.
    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        explicit Connection(asio::ip::tcp::socket socket, const terila::data::RegistryData& data,
            terila::entity::MobManager& mobs, terila::world::BlockWorld& world,
            const std::unordered_map<std::int32_t, std::int32_t>& item_block_state,
            std::shared_ptr<PlayerHub> hub,
            terila::world::trf::ChunkStore* store,
            int view_distance, int simulation_distance, int max_players,
            std::int32_t entity_id, int gamemode)
            : socket_(std::move(socket)), keep_alive_timer_(socket_.get_executor()), data_(data),
              mobs_(mobs), world_(world), item_block_state_(item_block_state),
              hub_(std::move(hub)), store_(store),
              view_distance_(view_distance), simulation_distance_(simulation_distance),
              max_players_(max_players), entity_id_(entity_id), gamemode_(gamemode)
        {}

        void start();

        void send(const std::vector<std::uint8_t>& data);

        // Called by the server tick for every connected player.
        void tick_entities();

        bool joined() const { return joined_; }

        terila::entity::PlayerPos player_position() const {
            return terila::entity::PlayerPos{ player_x_, player_y_, player_z_ };
        }

    private:
        // A peer that never completes a packet would otherwise grow the accumulator forever.
        static constexpr std::size_t kMaxReadAccum = 4 * 1024 * 1024;

        // Chunks per batch; the next batch waits until the previous one is on the wire.
        static constexpr int kChunksPerBatch = 32;

        // Keep a few packets in flight instead of stalling on every single write.
        static constexpr std::size_t kMaxWriteQueue = 4;

        static constexpr std::int32_t kPlayerAttackDamage = 4;

        struct ChunkPos {
            std::int32_t x = 0;
            std::int32_t z = 0;
        };

        struct KnownEntity {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            float yaw = 0.0f;
            std::uint32_t hurt_serial = 0;
            bool death_sent = false;
        };

        enum class State {
            Handshake,
            Status,
            Login,
            Configuration,
            Play
        };

        static const char* state_name(State s) noexcept;

        // --- connection.cpp: framing, socket lifecycle, packet dispatch -------------
        void process_packets();
        void do_read();
        void handle_read(const asio::error_code& error, std::size_t bytes_transferred);
        void handle_packet(std::int32_t id, const std::vector<std::uint8_t>& payload);
        void close();
        void do_write();
        void start_keep_alive();

        // --- per-state handlers ----------------------------------------------------
        void handle_handshake(std::int32_t id, const std::vector<std::uint8_t>& payload);
        void handle_status(std::int32_t id, const std::vector<std::uint8_t>& payload);
        void handle_login(std::int32_t id, const std::vector<std::uint8_t>& payload);
        void handle_configuration(std::int32_t id, const std::vector<std::uint8_t>& payload);
        void handle_play(std::int32_t id, const std::vector<std::uint8_t>& payload);

        // --- connection_chunks.cpp -------------------------------------------------
        static std::uint64_t chunk_key(std::int32_t cx, std::int32_t cz) noexcept;

        // Flat chunks are identical except for their coordinates, so one encode is enough.
        void build_chunk_template();

        // Served from TRF when the region holds the chunk, generated and persisted otherwise.
        std::vector<std::uint8_t> chunk_at(std::int32_t cx, std::int32_t cz);
        void persist_chunk(std::int32_t cx, std::int32_t cz,
            const std::vector<std::uint8_t>& encoded, bool edited);
        void persist_block_change(const terila::world::BlockPos& target);
        void queue_chunk_area(std::int32_t center_cx, std::int32_t center_cz);
        void forget_far_chunks(std::int32_t center_cx, std::int32_t center_cz);
        void pump_chunks();
        void update_player_chunk(double x, double z);

        // --- connection_world.cpp --------------------------------------------------
        std::optional<std::int32_t> block_state(std::string_view name) const;
        std::optional<std::int32_t> biome_state(std::string_view name) const;

        // The generated flat world, used when a position has no override yet.
        std::int32_t base_state_at(std::int32_t y) const;
        std::int32_t state_at(const terila::world::BlockPos& pos) const;
        bool inside_world(std::int32_t y) const;
        void send_block_change(const terila::world::BlockPos& pos, std::int32_t state,
            std::int32_t sequence);
        void handle_block_break(const terila::protocol::BlockPosValue& clicked,
            std::int32_t sequence);
        void handle_block_place(const terila::protocol::BlockPosValue& clicked,
            std::int32_t face, std::int32_t sequence);
        void refresh_held_item();

        // --- connection_entities.cpp -----------------------------------------------
        // Spawns and removes mobs as they enter or leave the view distance and sends
        // relative movement for the ones already known to the client.
        void send_entity_updates(const std::vector<terila::entity::Mob>& mobs);

        asio::ip::tcp::socket socket_;
        std::vector<std::uint8_t> read_accum_;
        std::array<std::uint8_t, 4096> read_buffer_;
        State state_ = State::Handshake;
        std::deque<std::vector<std::uint8_t>> write_queue_;
        bool writing_ = false;
        bool closed_ = false;
        asio::steady_timer keep_alive_timer_;
        std::int64_t next_keep_alive_id_ = 1;
        std::int32_t next_teleport_id_ = 1;
        std::int64_t pending_keep_alive_id_ = 0;
        bool keep_alive_answered_ = true;
        std::string username_;
        std::string remote_;
        bool joined_ = false;
        bool logged_first_entity_ = false;
        int view_distance_ = 10;
        int simulation_distance_ = 10;
        int max_players_ = 20;
        std::int32_t entity_id_ = 76;
        std::int32_t center_cx_ = 0;
        std::int32_t center_cz_ = 0;
        terila::world::flat::FlatSpec spec_;
        std::vector<std::uint8_t> chunk_template_;
        std::deque<ChunkPos> pending_chunks_;
        std::unordered_set<std::uint64_t> sent_chunks_;
        std::unordered_map<std::int32_t, KnownEntity> known_entities_;
        double player_x_ = 8.5;
        double player_y_ = 0.0;
        double player_z_ = 8.5;
        const terila::data::RegistryData& data_;
        terila::entity::MobManager& mobs_;
        terila::world::BlockWorld& world_;
        const std::unordered_map<std::int32_t, std::int32_t>& item_block_state_;
        std::shared_ptr<PlayerHub> hub_;
        terila::world::trf::ChunkStore* store_ = nullptr;
        std::array<std::uint8_t, 16> uuid_{};
        float yaw_ = 0.0f;
        float pitch_ = 0.0f;
        int gamemode_ = 1;
        std::int32_t air_state_ = 0;
        std::int32_t bedrock_state_ = 0;
        std::int32_t dirt_state_ = 0;
        std::int32_t grass_state_ = 0;
        std::int32_t carried_slot_ = 0;
        std::int32_t held_item_id_ = 0;
        std::unordered_map<std::int32_t, std::int32_t> creative_slots_;
    };

} // namespace terila::network
