#pragma once

#include <asio.hpp>

#include "data/recipe_book.hpp"
#include "data/registry_data.hpp"
#include "entity/item_drop.hpp"
#include "entity/mob.hpp"
#include "log/logger.hpp"
#include "network/player_hub.hpp"
#include "protocol/block.hpp"
#include "world/block_world.hpp"
#include "world/terrain_generator.hpp"
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
            terila::entity::ItemDropManager* drops,
            const std::unordered_map<std::int32_t, std::int32_t>& block_state_item,
            std::int32_t item_entity_type,
            const terila::data::RecipeBook* recipes,
            const std::unordered_map<std::int32_t, std::int32_t>& menu_blocks,
            const std::array<std::int32_t, 8>& furnace_states,
            std::int32_t furnace_item_id,
            std::uint32_t world_seed,
            int view_distance, int simulation_distance, int max_players,
            std::int32_t entity_id, int gamemode)
            : socket_(std::move(socket)), keep_alive_timer_(socket_.get_executor()), data_(data),
              mobs_(mobs), world_(world), item_block_state_(item_block_state),
              hub_(std::move(hub)), store_(store), drops_(drops),
              block_state_item_(block_state_item), item_entity_type_(item_entity_type),
              recipes_(recipes), menu_blocks_(menu_blocks), furnace_states_(furnace_states),
              furnace_item_id_(furnace_item_id), world_seed_(world_seed),
              view_distance_(view_distance), simulation_distance_(simulation_distance),
              max_players_(max_players), entity_id_(entity_id), gamemode_(gamemode)
        {}

        void start();

        void send(const std::vector<std::uint8_t>& data);

        // Called by the server tick for every connected player.
        void tick_entities();

        bool joined() const { return joined_; }

        terila::entity::PlayerPos player_position() const {
            return terila::entity::PlayerPos{ player_x_, player_y_, player_z_, entity_id_ };
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

        struct KnownDrop {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
        };

        struct InvSlot {
            std::int32_t item_id = 0;
            std::int32_t count = 0;
        };

        // Slots 9..44 of the player window: 0..26 main inventory, 27..35 the hotbar.
        static constexpr std::int32_t kFirstInventorySlot = 9;
        static constexpr std::int32_t kInventorySlots = 36;
        // Inventory index where the hotbar starts (protocol slot 36).
        static constexpr std::int32_t kMainInventorySlots = 27;
        static constexpr std::int32_t kMaxStackSize = 64;

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

        // --- gamemodes (0 survival, 1 creative, 2 adventure, 3 spectator) -----------
        void handle_change_gamemode(std::int32_t requested);
        void send_gamemode_state();
        bool can_break_blocks() const { return gamemode_ == 0 || gamemode_ == 1; }
        bool can_place_blocks() const { return gamemode_ == 0 || gamemode_ == 1; }
        bool instant_break() const { return gamemode_ == 1; }

        // --- connection_chunks.cpp -------------------------------------------------
        static std::uint64_t chunk_key(std::int32_t cx, std::int32_t cz) noexcept;

        // Resolves the block and biome ids the terrain generator needs.
        void init_terrain();

        // Served from TRF when the region holds the chunk, generated and persisted otherwise.
        std::vector<std::uint8_t> chunk_at(std::int32_t cx, std::int32_t cz);
        void apply_loaded_chunk(const terila::world::Chunk& chunk);
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
        std::int32_t state_at(const terila::world::BlockPos& pos) const;
        bool inside_world(std::int32_t y) const;
        void send_block_change(const terila::world::BlockPos& pos, std::int32_t state,
            std::int32_t sequence);
        void handle_block_break(const terila::protocol::BlockPosValue& clicked,
            std::int32_t sequence);
        void handle_block_place(const terila::protocol::BlockPosValue& clicked,
            std::int32_t face, std::int32_t sequence);
        void refresh_held_item();

        // --- dropped items ---------------------------------------------------------
        // Surface a broken block falls onto: the block above the grass layer.
        double drop_ground_y() const;
        void spawn_block_drop(const terila::world::BlockPos& pos, std::int32_t state);
        void handle_pick_block(const terila::protocol::BlockPosValue& clicked);
        void give_hotbar_item(std::int32_t item_id);
        void send_drop_updates();

        // --- connection_inventory.cpp ----------------------------------------------
        // Puts collected drops into the player inventory, merging into existing stacks
        // first and telling the client about every slot it changes.
        bool add_to_inventory(std::int32_t item_id, std::int32_t count);
        void send_inventory_slot(std::int32_t slot);
        void set_inventory_slot(std::int32_t slot, std::int32_t item_id, std::int32_t count);

        // --- connection_menu.cpp: crafting table and furnace windows --------------
        struct MenuSlot {
            std::int32_t item_id = 0;
            std::int32_t count = 0;
        };

        enum MenuKind { kMenuNone = 0, kMenuCrafting = 1, kMenuFurnace = 2 };

        void open_menu(int kind, std::string_view title);
        void close_menu();
        void send_menu_content();
        void send_menu_slot(std::int32_t slot);
        void handle_container_click(const std::vector<std::uint8_t>& payload);
        void handle_container_close(std::int32_t window_id);
        void recompute_crafting_result();
        void tick_menu();
        void clone_slot(std::int32_t slot);
        void drag_click(std::int32_t button, std::int32_t slot, bool outside);
        bool place_one_from_cursor(std::int32_t slot);
        void quick_move(std::int32_t slot);
        std::int32_t menu_player_room(std::int32_t item_id) const;
        std::int32_t add_to_menu_player(std::int32_t item_id, std::int32_t count);
        void send_furnace_data(std::int32_t data_id, std::int32_t value);
        int menu_kind_for_state(std::int32_t state) const;
        std::int32_t menu_player_offset() const {
            return menu_kind_ == kMenuCrafting ? 10 : 3;
        }

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
        terila::world::terrain::Generator terrain_;
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
        terila::entity::ItemDropManager* drops_ = nullptr;
        const std::unordered_map<std::int32_t, std::int32_t>& block_state_item_;
        std::int32_t item_entity_type_ = 0;
        std::unordered_map<std::int32_t, KnownDrop> known_drops_;
        std::array<InvSlot, kInventorySlots> inventory_{};
        bool menu_open_ = false;
        MenuKind menu_kind_ = kMenuNone;
        std::int32_t menu_id_ = 0;
        std::int32_t menu_state_id_ = 0;
        std::vector<MenuSlot> menu_slots_;
        MenuSlot cursor_{};
        bool dragging_ = false;
        int drag_mode_ = 0;
        std::vector<std::int32_t> drag_slots_;
        int furnace_burn_ = 0;
        int furnace_burn_total_ = 0;
        int furnace_cook_ = 0;
        std::int32_t last_burn_ = -1;
        std::int32_t last_burn_total_ = -1;
        std::int32_t last_cook_ = -1;
        std::int32_t last_cook_total_ = -1;
        const terila::data::RecipeBook* recipes_ = nullptr;
        const std::unordered_map<std::int32_t, std::int32_t>& menu_blocks_;
        // Furnace variants indexed [facing * 2 + lit]: 0 north, 1 south, 2 west, 3 east.
        const std::array<std::int32_t, 8>& furnace_states_;
        std::int32_t furnace_item_id_ = 0;
        std::uint32_t world_seed_ = 0;
        std::array<std::uint8_t, 16> uuid_{};
        float yaw_ = 0.0f;
        float pitch_ = 0.0f;
        int gamemode_ = 1;
        std::int32_t air_state_ = 0;
        std::int32_t carried_slot_ = 0;
        std::int32_t held_item_id_ = 0;
        std::unordered_map<std::int32_t, std::int32_t> creative_slots_;
    };

} // namespace terila::network
