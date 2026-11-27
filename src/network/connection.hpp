#pragma once

#include <asio.hpp>

#include "async/task.hpp"
#include "data/recipe_book.hpp"
#include "data/registry_data.hpp"
#include "entity/item_drop.hpp"
#include "entity/mob.hpp"
#include "item/components.hpp"
#include "log/logger.hpp"
#include "network/player_hub.hpp"
#include "protocol/block.hpp"
#include "protocol/player_state.hpp"
#include "world/block_world.hpp"
#include "world/falling_blocks.hpp"
#include "world/fluid_flow.hpp"
#include "commands/registry.hpp"
#include "world/world_context.hpp"
#include "world/chunk_jobs.hpp"
#include "world/world_clock.hpp"
#include "world/world_generator.hpp"
#include "world/trf/trf_store.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace terila::network {

    // The three bucket items, so pouring and scooping fluids never guesses from names.
    struct FluidItems {
        std::int32_t bucket = 0;
        std::int32_t water_bucket = 0;
        std::int32_t lava_bucket = 0;
    };

    // One TCP peer. The class only declares the state machine: framing and the handshake,
    // status, login, configuration and play handlers live in their own translation units.
    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        explicit Connection(asio::ip::tcp::socket socket, const terila::data::RegistryData& data,
            terila::entity::MobManager& mobs, terila::world::BlockWorld& world,
            terila::world::FallingBlocks& falling,
            terila::world::FluidFlow& fluids,
            const std::unordered_map<std::int32_t, std::int32_t>& item_block_state,
            std::shared_ptr<PlayerHub> hub,
            const std::array<terila::world::trf::ChunkStore*, terila::world::kDimensionCount>& stores,
            terila::entity::ItemDropManager* drops,
            const std::unordered_map<std::int32_t, std::int32_t>& block_state_item,
            std::int32_t item_entity_type,
            const terila::data::RecipeBook* recipes,
            const std::unordered_map<std::int32_t, std::int32_t>& menu_blocks,
            const std::array<std::int32_t, 8>& furnace_states,
            std::int32_t furnace_item_id,
            FluidItems fluid_items,
            std::uint32_t world_seed, terila::world::WorldType world_type,
            std::int32_t spawn_x, std::int32_t spawn_z,
            int view_distance, int simulation_distance, int max_players,
            std::int32_t entity_id, int gamemode, int difficulty, bool hardcore,
            std::shared_ptr<terila::world::WorldClock> clock, int sea_level)
            : socket_(std::move(socket)), keep_alive_timer_(socket_.get_executor()), data_(data),
              mobs_(mobs), world_(world), falling_(falling), fluids_(fluids), item_block_state_(item_block_state),
              hub_(std::move(hub)), stores_(stores), store_(stores[0]), drops_(drops),
              block_state_item_(block_state_item), item_entity_type_(item_entity_type),
              recipes_(recipes), menu_blocks_(menu_blocks), furnace_states_(furnace_states),
              furnace_item_id_(furnace_item_id), fluid_items_(fluid_items),
              world_seed_(world_seed),
              world_type_(world_type),               spawn_x_(spawn_x), spawn_z_(spawn_z),
              terrain_(&terila::world::WorldContext::shared().generator()),
              view_distance_(view_distance), simulation_distance_(simulation_distance),
              max_players_(max_players), entity_id_(entity_id), gamemode_(gamemode),
              difficulty_(difficulty), hardcore_(hardcore),
              clock_(std::move(clock)), sea_level_(sea_level)
        {}

        void start();

        void send(const std::vector<std::uint8_t>& data);
        // A caller that is done with its buffer hands it over instead of paying for a copy.
        void send(std::vector<std::uint8_t>&& data);

        // Called by the server tick for every connected player.
        void tick_entities();
        // Sends the world clock once a second, which is what moves the client's sky.
        void tick_time();
        void send_time();

        // A mob hit on this player. False when it did nothing (creative, spectator, already
        // dead).
        bool damage_player(float amount, const std::string& cause);
        // /kill: an unconditional hit, so a creative player dies to it as well.
        bool kill_player(const std::string& cause);
        void send_health();
        // The death screen, the combat message and the state behind them, shared by a hit
        // that empties the health bar and by /kill.
        void die(const std::string& cause);
        // Puts the player back at the world spawn after the death screen's respawn button.
        void respawn_player();
        bool dead() const { return dead_; }

        // Persists a few of the chunks generated while streaming, so the initial chunk
        // burst is not serialised behind region-file writes on the IO thread.
        void flush_pending_saves(std::size_t budget);

        // The tick drives the chunk stream. A batch waiting on a worker while the socket
        // is quiet has nothing else to resume it, so the stream is pumped every tick
        // instead of only when a write completes.
        void tick_chunks() { pump_chunks(); }

        // The block simulation moved a block in the shared overlay: tell this client when it
        // already holds the column, and save a column it touched once per cascade.
        void send_world_block(const terila::world::BlockPos& pos, std::int32_t state);
        void persist_chunk_edits(std::int32_t cx, std::int32_t cz);

        bool joined() const { return joined_; }

        // True while a burst of chunks (a login or a dimension change) is still being sent.
        // The tick keeps the expensive world sampling of the natural spawner off the io
        // thread until the player is in, so the first view distance arrives first.
        bool chunks_streaming() const noexcept {
            return !pending_chunks_.empty() || !chunk_stream_.finished();
        }

        terila::world::DimensionKind dimension() const noexcept { return dimension_; }

        terila::entity::PlayerPos player_position() const {
            terila::entity::PlayerPos pos;
            pos.x = player_x_;
            pos.y = player_y_;
            pos.z = player_z_;
            pos.id = entity_id_;
            pos.held_item = held_item_name_;
            pos.creative = gamemode_ == 1 || gamemode_ == 3;
            return pos;
        }

    private:
        // A peer that never completes a packet would otherwise grow the accumulator forever.
        static constexpr std::size_t kMaxReadAccum = 4 * 1024 * 1024;

        // Chunks handed to the workers at once; a deeper queue keeps them fed while the
        // earlier chunks of the batch are still being framed and written.
        static constexpr int kChunksPerBatch = 48;
        // Chunk packets are already framed, so a few of them share one write.
        static constexpr std::size_t kChunkGroupBytes = 64 * 1024;

        // Keep a few packets in flight instead of stalling on every single write.
        static constexpr std::size_t kMaxWriteQueue = 8;
        // Saves drain a few per tick, so a player who explores faster than the region file
        // accepts would otherwise queue the whole world in memory.
        static constexpr std::size_t kMaxPendingSaves = 512;

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
            float head_yaw = 0.0f;
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
            terila::item::ComponentsPtr components;
        };

        // Slots 9..44 of the player window: 0..26 main inventory, 27..35 the hotbar.
        static constexpr std::int32_t kFirstInventorySlot = 9;
        static constexpr std::int32_t kInventorySlots = 36;
        // Inventory index where the hotbar starts (protocol slot 36).
        static constexpr std::int32_t kMainInventorySlots = 27;
        static constexpr std::int32_t kMaxStackSize = 64;
        // /execute can nest (/execute as @s run execute ...), and an unbounded chain would
        // recurse until the stack runs out.
        static constexpr std::uint32_t kMaxCommandDepth = 8;

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

        // Floor division by 16: >> rounds towards zero for negative columns.
        static std::int32_t chunk_coordinate(std::int32_t block) {
            return block >= 0 ? block / 16 : -((-block + 15) / 16);
        }

        // Resolves the block and biome ids the terrain generator needs.
        void init_terrain();

        // Served from TRF when the region holds the chunk, generated and persisted otherwise.
        std::vector<std::uint8_t> chunk_at(std::int32_t cx, std::int32_t cz);
        // The same chunk, reusing a column the worker pool already generated.
        std::vector<std::uint8_t> chunk_at(std::int32_t cx, std::int32_t cz,
            std::optional<terila::world::PreparedChunk> prepared);
        // The baseline is passed in because the caller may already hold the generated
        // chunk, and generating it twice per load is the expensive part.
        void apply_loaded_chunk(const terila::world::Chunk& chunk,
            const terila::world::Chunk& generated);
        void persist_chunk(std::int32_t cx, std::int32_t cz,
            const std::vector<std::uint8_t>& encoded, bool edited,
            terila::world::DimensionKind kind);
        // Chunks generated while streaming are written back on later ticks.
        void queue_persist(std::int32_t cx, std::int32_t cz,
            const std::vector<std::uint8_t>& encoded, bool edited);
        void persist_block_change(const terila::world::BlockPos& target);
        // Light belongs to a chunk, so an edited chunk has to be lit again before it is
        // encoded: a dug-out cell otherwise keeps the darkness it had while it was solid.
        void relight_chunk(terila::world::Chunk& chunk);
        void queue_chunk_area(std::int32_t center_cx, std::int32_t center_cz);
        void forget_far_chunks(std::int32_t center_cx, std::int32_t center_cz);
        // The stream is a coroutine: one step per pump, so a full write queue only pauses
        // it instead of losing the batch it was in the middle of.
        terila::async::Task<void> stream_chunks();
        void pump_chunks();
        // Says why the chunk stream ended, so a failing column does not look like a client
        // that simply stopped asking for terrain.
        void report_chunk_stream_failure();
        // Handed to the chunk workers so the stream is resumed on the io thread the moment
        // a chunk is ready. Polling once per tick cost up to 50 ms per chunk, which is what
        // made a whole view distance take forty seconds instead of a couple.
        std::function<void()> chunk_completion();
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

        // --- nether portal (connection_world.cpp) -----------------------------------
        // Flint and steel (or a fire charge) opens the obsidian frame it was used in, or
        // lights a fire when there is no frame. False when the held item is not an igniter,
        // so the normal placement path still runs.
        bool handle_ignite(const terila::protocol::BlockPosValue& clicked, std::int32_t face,
            std::int32_t sequence);
        // Resolves the item and block ids the portal paths need, once per connection.
        void resolve_portal_ids();

        // --- fluids (connection_world.cpp) -----------------------------------------
        // A bucket pours its source into the clicked cell and turns into the empty bucket.
        void place_from_bucket(const terila::protocol::BlockPosValue& clicked, std::int32_t face,
            std::int32_t sequence, terila::world::FluidFlow::Kind kind);
        void pickup_with_bucket(const terila::protocol::BlockPosValue& clicked,
            std::int32_t sequence);
        void set_held_bucket(std::int32_t item_id);

        // --- survival breaking (connection_world.cpp) -------------------------------
        // Survival waits out the block's hardness; creative still removes it at once.
        int break_ticks_for(std::int32_t state) const;
        void start_breaking(const terila::world::BlockPos& pos);
        void abort_breaking();
        void finish_breaking(const terila::world::BlockPos& pos);
        void tick_breaking();
        void break_now(const terila::world::BlockPos& target, std::int32_t sequence);

        // --- dropped items ---------------------------------------------------------
        // Surface a broken block falls onto: the block above the grass layer.
        double drop_ground_y(std::int32_t x, std::int32_t z) const;
        void spawn_block_drop(const terila::world::BlockPos& pos, std::int32_t state);
        void handle_pick_block(const terila::protocol::BlockPosValue& clicked);
        void give_hotbar_item(std::int32_t item_id);
        void send_drop_updates();

        // --- connection_inventory.cpp ----------------------------------------------
        // Puts collected drops into the player inventory, merging into existing stacks
        // first and telling the client about every slot it changes.
        bool add_to_inventory(std::int32_t item_id, std::int32_t count,
            const terila::item::ComponentsPtr& components = nullptr);
        void send_inventory_slot(std::int32_t slot);
        void set_inventory_slot(std::int32_t slot, std::int32_t item_id, std::int32_t count);

        // --- connection_menu.cpp: crafting table and furnace windows --------------
        struct MenuSlot {
            std::int32_t item_id = 0;
            std::int32_t count = 0;
            terila::item::ComponentsPtr components;
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
        std::int32_t menu_player_room(std::int32_t item_id,
            const terila::item::ComponentsPtr& components) const;
        std::int32_t add_to_menu_player(std::int32_t item_id, std::int32_t count,
            const terila::item::ComponentsPtr& components = nullptr);
        void send_furnace_data(std::int32_t data_id, std::int32_t value);
        int menu_kind_for_state(std::int32_t state) const;
        std::int32_t menu_player_offset() const {
            return menu_kind_ == kMenuCrafting ? 10 : 3;
        }

        // --- connection_health.cpp -------------------------------------------------
        void handle_client_command(std::int32_t action);
        void handle_interact(const std::vector<std::uint8_t>& payload);

        // --- connection_commands.cpp -----------------------------------------------
        friend class CommandBridge;

        void handle_command(const std::string& command);
        void send_system_message(std::string_view text);
        bool give_item(std::int32_t item_id, std::int32_t count,
            const terila::item::ComponentsPtr& components);

        // A command acts on another player through the hub; these are the local half of
        // that path, and the bridge in connection_commands.cpp chooses between them.
        bool teleport_player(double x, double y, double z, float yaw, float pitch);
        // Moves this player to another dimension and streams that world instead. False when
        // the dimension is the one already loaded, or when its generator is unavailable.
        bool enter_dimension(terila::world::DimensionKind kind, double x, double y, double z,
            float yaw, float pitch);
        bool apply_gamemode(int mode);
        bool tell_player(std::string_view text);
        bool summon_entity(std::string_view type, double x, double y, double z);

        // Built once, on start: the grammar and the tree both come from it.
        std::unique_ptr<commands::Registry> registry_;

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
        // Block column every player starts at, chosen by the world generator.
        std::int32_t spawn_x_ = 8;
        std::int32_t spawn_z_ = 8;
        // Points at the shared generator of the dimension this player is standing in.
        terila::world::DimensionKind dimension_ = terila::world::DimensionKind::Overworld;
        terila::world::WorldGenerator* terrain_ = nullptr;
        // Bumped whenever the dimension changes, so a batch of the old world that is still
        // in flight can tell it is stale and stop instead of streaming over the new one.
        std::uint32_t chunk_epoch_ = 0;
        // Shared with the chunk jobs this connection queued: it is cleared when the
        // connection closes, so the workers stop generating a view distance nobody will see.
        std::shared_ptr<std::atomic<bool>> chunk_work_alive_ =
            std::make_shared<std::atomic<bool>>(true);
        std::deque<ChunkPos> pending_chunks_;
        // Keeps the coroutine frame of the chunk stream, so an unfinished batch survives a
        // full write queue.
        terila::async::Stream chunk_stream_;
        struct PendingSave {
            std::int32_t x = 0;
            std::int32_t z = 0;
            std::vector<std::uint8_t> bytes;
            // A column that carries player edits must not be written back as pristine.
            bool edited = false;
            // A save queued before a dimension change still belongs to the old world.
            terila::world::DimensionKind dimension = terila::world::DimensionKind::Overworld;
        };
        std::deque<PendingSave> pending_saves_;
        std::unordered_set<std::uint64_t> sent_chunks_;
        std::unordered_map<std::int32_t, KnownEntity> known_entities_;
        // Reused every tick: the entity and drop streams both need a set of the ids that
        // are still around, and rebuilding it twenty times a second churns the allocator.
        std::unordered_set<std::int32_t> alive_scratch_;
        double player_x_ = 8.5;
        double player_y_ = 0.0;
        double player_z_ = 8.5;
        const terila::data::RegistryData& data_;
        terila::entity::MobManager& mobs_;
        terila::world::BlockWorld& world_;
        terila::world::FallingBlocks& falling_;
        terila::world::FluidFlow& fluids_;
        const std::unordered_map<std::int32_t, std::int32_t>& item_block_state_;
        std::shared_ptr<PlayerHub> hub_;
        std::array<terila::world::trf::ChunkStore*, terila::world::kDimensionCount> stores_{};
        // The store of the dimension this player currently stands in.
        terila::world::trf::ChunkStore* store_ = nullptr;
        terila::entity::ItemDropManager* drops_ = nullptr;
        const std::unordered_map<std::int32_t, std::int32_t>& block_state_item_;
        std::int32_t item_entity_type_ = 0;
        std::unordered_map<std::int32_t, KnownDrop> known_drops_;
        std::array<InvSlot, kInventorySlots> inventory_{};
        // The block a survival player is holding the button down on, and how far that dig
        // has got. Cleared whenever the dig stops for any reason.
        terila::world::BlockPos breaking_pos_{};
        std::int32_t breaking_state_ = 0;
        int break_progress_ = 0;
        int break_ticks_ = 0;
        bool breaking_ = false;
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
        FluidItems fluid_items_;
        std::uint32_t world_seed_ = 0;
        terila::world::WorldType world_type_ = terila::world::WorldType::Terrain;
        terila::item::ComponentIds component_ids_{};
        std::array<std::uint8_t, 16> uuid_{};
        // The signed skin properties of the account, sent with its player list entries.
        std::vector<protocol::ProfileProperty> profile_properties_;
        float yaw_ = 0.0f;
        float pitch_ = 0.0f;
        int gamemode_ = 1;
        // 0 peaceful, 1 easy, 2 normal, 3 hard, the same numbering the packet uses.
        int difficulty_ = 2;
        bool hardcore_ = false;
        float health_ = 20.0f;
        std::int32_t food_ = 20;
        float saturation_ = 5.0f;
        bool dead_ = false;
        int time_sync_ticks_ = 0;
        int regen_ticks_ = 0;
        std::string held_item_name_;
        std::shared_ptr<terila::world::WorldClock> clock_;
        int sea_level_ = 63;
        std::int32_t air_state_ = 0;
        // init_terrain() runs once per dimension: resolving the ids and the generator on
        // every streamed chunk made a 1089 chunk view distance do 1089 times the same work.
        bool terrain_ready_ = false;
        std::int32_t carried_slot_ = 0;
        std::int32_t held_item_id_ = 0;
        std::int32_t flint_and_steel_item_ = 0;
        std::int32_t fire_charge_item_ = 0;
        std::int32_t obsidian_state_ = -1;
        std::int32_t portal_state_x_ = -1;
        std::int32_t portal_state_z_ = -1;
        std::int32_t fire_state_ = -1;
        bool portal_ids_ready_ = false;
        std::unordered_map<std::int32_t, std::int32_t> creative_slots_;
    };

} // namespace terila::network
