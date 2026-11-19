#pragma once

// Water and lava flow on the block overlay the way the falling blocks do: no fluid entities
// and no scheduled block ticks, only a queue of cells re-evaluated after the fluid's own
// tick delay (FlowingFluid.getTickDelay: five game ticks for water, thirty for lava).
//
// A cell stores the legacy block state level of its fluid. Level 0 is a source, 1..7 is a
// flow whose amount is 8 - level, and 8..15 is a falling column carrying the same amounts
// (8 is a full column, 15 the thinnest). Spreading loses the fluid's drop-off per block, so
// water steps 1,2,3,4,5,6,7 while lava keeps 2,4,6 - three blocks out of a source.
//
// The rules are the ones FlowingFluid implements: every tick a cell recomputes its level
// from its neighbours, a cell that can flow down becomes falling fluid and does not fan out
// sideways until it lands, sideways spreading prefers the shortest path to a drop within the
// fluid's slope find distance, two sources over solid ground turn the gap into a source, and
// a cell nobody feeds any more drains away. The two fluids never share a cell: water turns
// lava into obsidian (source) or cobblestone (flow), and lava poured onto water leaves
// stone.

#include "world/block_world.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace terila::world {

    class FluidFlow {
    public:
        enum class Kind : int {
            Water = 0,
            Lava = 1
        };

        static constexpr int kKinds = 2;
        // Legacy block state levels: 0 source, 1..7 flowing, 8..15 falling.
        static constexpr int kLevels = 16;
        static constexpr int kFullAmount = 8;
        static constexpr int kNoPath = 1000;
        static constexpr int kDefaultDelay = 5;

        struct Fluid {
            Fluid() { states.fill(-1); }

            // Legacy level to the block state carrying it, straight from the registry.
            std::array<std::int32_t, kLevels> states;
            int drop_off = 1;
            int slope_find = 4;
            int tick_delay = kDefaultDelay;
            bool source_conversion = true;
        };

        using StateAt = std::function<std::int32_t(const BlockPos&)>;
        using ChangeSink = std::function<void(const BlockPos&, std::int32_t)>;

        void set_world(BlockWorld* world, std::int32_t air_state, std::int32_t min_y,
            std::int32_t max_y) {
            world_ = world;
            air_state_ = air_state;
            min_y_ = min_y;
            max_y_ = max_y;
        }

        void set_fluid(Kind kind, Fluid fluid) {
            fluids_[static_cast<std::size_t>(kind)] = std::move(fluid);
        }

        // Cells a fluid may occupy: the game's #minecraft:washed_away_by_fluids tag (air,
        // plants, snow layers and the fluids themselves), so nothing is guessed from ids.
        void set_replaceable(std::unordered_set<std::int32_t> replaceable) {
            replaceable_ = std::move(replaceable);
        }

        // What contact between the two fluids leaves behind: obsidian from a lava source,
        // cobblestone from a lava flow, and stone where lava runs onto water.
        void set_solids(std::int32_t obsidian, std::int32_t cobblestone, std::int32_t stone) {
            obsidian_ = obsidian;
            cobblestone_ = cobblestone;
            stone_ = stone;
        }

        void set_state_at(StateAt state_at) { state_at_ = std::move(state_at); }
        void set_change_sink(ChangeSink sink) { sink_ = std::move(sink); }

        int kind_of(std::int32_t state) const {
            for (int kind = 0; kind < kKinds; ++kind) {
                const auto& states = fluids_[static_cast<std::size_t>(kind)].states;
                for (const auto candidate : states) {
                    if (candidate >= 0 && candidate == state) {
                        return kind;
                    }
                }
            }
            return -1;
        }

        int level_of(std::int32_t state) const {
            for (int kind = 0; kind < kKinds; ++kind) {
                const auto& states = fluids_[static_cast<std::size_t>(kind)].states;
                for (std::size_t level = 0; level < states.size(); ++level) {
                    if (states[level] >= 0 && states[level] == state) {
                        return static_cast<int>(level);
                    }
                }
            }
            return -1;
        }

        bool is_fluid(std::int32_t state) const { return kind_of(state) >= 0; }
        bool is_source(std::int32_t state) const { return level_of(state) == 0; }
        std::size_t pending() const noexcept { return queue_.size(); }

        // FlowingFluid.getLegacyLevel: a source is level 0, a flow carries 8 - amount and a
        // falling cell adds 8 to the same amount encoding.
        static int amount_of(int level) {
            if (level <= 0) {
                return kFullAmount;
            }
            return level < kFullAmount ? kFullAmount - level : 2 * kFullAmount - level;
        }

        static bool is_falling(int level) { return level >= kFullAmount; }

        int level_for(int amount, bool falling) const {
            if (amount <= 0) {
                return -1;
            }
            const int capped = amount > kFullAmount ? kFullAmount : amount;
            return falling ? 2 * kFullAmount - capped : kFullAmount - capped;
        }

        // Cells that can take fluid: the washed-away tag plus any cell that already carries
        // one of the two fluids (the tag lists the fluids themselves as well).
        bool can_hold(std::int32_t state) const {
            return replaceable_.find(state) != replaceable_.end() || kind_of(state) >= 0;
        }

        bool can_hold(const BlockPos& pos) const {
            return inside(pos) && can_hold(state_at(pos));
        }

        // A block changed: the cell, its four sides and the cells above and below all get a
        // fluid tick, which is the neighbour update a liquid block receives.
        void block_changed(const BlockPos& pos) {
            schedule(pos);
            for (int dir = 0; dir < 4; ++dir) {
                schedule(neighbour(pos, dir));
            }
            schedule(BlockPos{ pos.x, pos.y + 1, pos.z });
            schedule(BlockPos{ pos.x, pos.y - 1, pos.z });
        }

        // A bucket emptied into this cell. Reports false when the cell cannot take fluid,
        // so the caller can refuse the placement instead of silently dropping it.
        bool place_source(const BlockPos& pos, Kind kind) {
            if (world_ == nullptr || !inside(pos)) {
                return false;
            }
            if (!can_hold(state_at(pos)) || kind_of(state_at(pos)) >= 0) {
                return false;
            }
            const std::int32_t state = fluids_[static_cast<std::size_t>(kind)].states[0];
            if (state < 0) {
                return false;
            }
            write_block(pos, state);
            return true;
        }

        // A bucket scooped a cell: only a source can be taken, flowing fluid stays.
        bool remove_source(const BlockPos& pos, Kind& removed) {
            if (!inside(pos)) {
                return false;
            }
            const int level = level_of(state_at(pos));
            if (level != 0) {
                return false;
            }
            removed = static_cast<Kind>(kind_of(state_at(pos)));
            write_block(pos, air_state_);
            return true;
        }

        void tick() {
            if (queue_.empty()) {
                return;
            }
            batch_.assign(queue_.begin(), queue_.end());
            queue_.clear();
            queued_.clear();
            touched_.clear();

            for (auto& job : batch_) {
                // Scheduled for the fluid's tick delay: the cell is re-read on that tick.
                if (--job.delay > 0) {
                    enqueue(job);
                    continue;
                }
                // A cell this tick already filled is only re-read on its own new tick, so
                // the front advances one block per fluid tick instead of cascading.
                if (touched_.count(key_of(job.pos)) != 0) {
                    continue;
                }
                step(job.pos);
            }
        }

    private:
        struct Job {
            BlockPos pos;
            int delay = 0;
        };

        struct Cell {
            int kind = -1;
            int level = 0;
        };

        static std::uint64_t key_of(const BlockPos& pos) noexcept {
            const auto x = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(pos.x) & 0x3FFFFFFu);
            const auto z = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(pos.z) & 0x3FFFFFFu);
            const auto y = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(pos.y + 4096) & 0xFFFu);
            return (x << 38) | (z << 12) | y;
        }

        static constexpr int kDx[4] = { 1, -1, 0, 0 };
        static constexpr int kDz[4] = { 0, 0, 1, -1 };

        static BlockPos neighbour(const BlockPos& pos, int dir) noexcept {
            return BlockPos{ pos.x + kDx[dir], pos.y, pos.z + kDz[dir] };
        }

        static BlockPos above(const BlockPos& pos) noexcept {
            return BlockPos{ pos.x, pos.y + 1, pos.z };
        }

        static BlockPos below(const BlockPos& pos) noexcept {
            return BlockPos{ pos.x, pos.y - 1, pos.z };
        }

        static int opposite(int dir) noexcept { return dir < 2 ? 1 - dir : 5 - dir; }

        bool inside(const BlockPos& pos) const {
            return pos.y >= min_y_ && pos.y <= max_y_;
        }

        std::int32_t state_at(const BlockPos& pos) const {
            const std::int32_t generated = state_at_ ? state_at_(pos) : air_state_;
            return world_ == nullptr ? generated : world_->get(pos, generated);
        }

        Cell cell_at(const BlockPos& pos) const {
            const std::int32_t state = state_at(pos);
            Cell cell;
            cell.kind = kind_of(state);
            if (cell.kind >= 0) {
                cell.level = level_of(state);
            }
            return cell;
        }

        Cell cell_of(std::int32_t state) const {
            Cell cell;
            cell.kind = kind_of(state);
            if (cell.kind >= 0) {
                cell.level = level_of(state);
            }
            return cell;
        }

        const Fluid& fluid(Kind kind) const {
            return fluids_[static_cast<std::size_t>(kind)];
        }

        void enqueue(const Job& job) {
            if (!queued_.insert(key_of(job.pos)).second) {
                return;
            }
            queue_.push_back(job);
        }

        void schedule(const BlockPos& pos) {
            if (!inside(pos)) {
                return;
            }
            const int kind = cell_at(pos).kind;
            const int delay = kind < 0 ? kDefaultDelay : fluid(static_cast<Kind>(kind)).tick_delay;
            enqueue(Job{ pos, delay });
        }

        void write_block(const BlockPos& pos, std::int32_t state) {
            if (world_ == nullptr || state < 0 || state_at(pos) == state) {
                return;
            }
            world_->set(pos, state);
            touched_.insert(key_of(pos));
            if (sink_) {
                sink_(pos, state);
            }
            // Everything around the new fluid has to look at it again.
            block_changed(pos);
        }

        bool write_fluid(const BlockPos& pos, Kind kind, int level) {
            if (level < 0 || level >= kLevels) {
                return false;
            }
            const std::int32_t state = fluid(kind).states[static_cast<std::size_t>(level)];
            if (state < 0) {
                return false;
            }
            write_block(pos, state);
            return true;
        }

        // FlowingFluid.canMaybePassThrough: a fluid may move into a cell that can hold fluid
        // and is not already a source of the same fluid.
        bool can_pass(const BlockPos& pos, Kind kind) const {
            if (!inside(pos)) {
                return false;
            }
            const Cell cell = cell_at(pos);
            if (cell.kind == static_cast<int>(kind) && cell.level == 0) {
                return false;
            }
            return cell.kind >= 0 || can_hold(state_at(pos));
        }

        // Water and lava never share a cell. Water reaching lava sets it solid (obsidian from
        // a source, cobblestone from a flow) and lava running onto water leaves stone behind.
        bool solidify(const BlockPos& pos, Kind incoming) {
            const Cell target = cell_at(pos);
            if (target.kind < 0 || target.kind == static_cast<int>(incoming)) {
                return false;
            }
            if (incoming == Kind::Water) {
                write_block(pos, target.level == 0 ? obsidian_ : cobblestone_);
            }
            else {
                write_block(pos, stone_);
            }
            return true;
        }

        // FluidState.canBeReplacedWith: an empty cell always accepts, the same fluid never
        // (a flowing cell recomputes its own level), and between the two fluids only lava
        // below water - straight down - or lava sinking into tall enough water.
        bool can_replace_with(std::int32_t state, Kind kind, bool down) const {
            const Cell target = cell_of(state);
            if (target.kind < 0) {
                return true;
            }
            if (target.kind == static_cast<int>(kind)) {
                return false;
            }
            if (kind == Kind::Water) {
                return down;
            }
            return amount_of(target.level) >= 4;
        }

        // FlowingFluid.getNewLiquid: the level a cell takes from its neighbours.
        int new_level(const BlockPos& pos, Kind kind) const {
            int highest = 0;
            int sources = 0;
            for (int dir = 0; dir < 4; ++dir) {
                const Cell side = cell_at(neighbour(pos, dir));
                if (side.kind != static_cast<int>(kind)) {
                    continue;
                }
                if (side.level == 0) {
                    ++sources;
                }
                const int amount = amount_of(side.level);
                if (amount > highest) {
                    highest = amount;
                }
            }

            if (sources >= 2 && fluid(kind).source_conversion) {
                const Cell support = cell_at(below(pos));
                const bool solid = support.kind < 0 && !can_hold(state_at(below(pos)));
                const bool source = support.kind == static_cast<int>(kind) && support.level == 0;
                if (solid || source) {
                    return 0;
                }
            }

            if (cell_at(above(pos)).kind == static_cast<int>(kind)) {
                return level_for(kFullAmount, true);
            }

            return level_for(highest - fluid(kind).drop_off, false);
        }

        // FlowingFluid.isWaterHole: fluid would run down into this cell.
        bool is_hole(const BlockPos& pos, Kind kind) const {
            const BlockPos down = below(pos);
            if (!inside(down)) {
                return false;
            }
            const Cell target = cell_at(down);
            if (target.kind == static_cast<int>(kind)) {
                return true;
            }
            return can_hold(state_at(down));
        }

        // FlowingFluid.getSlopeDistance: how far the nearest drop is along a straight run,
        // never walking back where it came from and never past the fluid's slope find.
        int slope_distance(const BlockPos& pos, int from, int depth, Kind kind) const {
            int lowest = kNoPath;
            for (int dir = 0; dir < 4; ++dir) {
                if (dir == opposite(from)) {
                    continue;
                }
                const BlockPos next = neighbour(pos, dir);
                if (!can_pass(next, kind)) {
                    continue;
                }
                if (is_hole(next, kind)) {
                    return depth;
                }
                if (depth < fluid(kind).slope_find) {
                    const int found = slope_distance(next, dir, depth + 1, kind);
                    if (found < lowest) {
                        lowest = found;
                    }
                }
            }
            return lowest;
        }

        // How many of the four sides hold a source of this fluid.
        int source_neighbours(const BlockPos& pos, Kind kind) const {
            int count = 0;
            for (int dir = 0; dir < 4; ++dir) {
                const Cell side = cell_at(neighbour(pos, dir));
                if (side.kind == static_cast<int>(kind) && side.level == 0) {
                    ++count;
                }
            }
            return count;
        }

        // LiquidBlock.shouldSpreadLiquid: lava that sees water sets itself solid before it
        // flows any further.
        bool lava_touches_water(const BlockPos& pos) const {
            if (cell_at(above(pos)).kind == static_cast<int>(Kind::Water)) {
                return true;
            }
            for (int dir = 0; dir < 4; ++dir) {
                if (cell_at(neighbour(pos, dir)).kind == static_cast<int>(Kind::Water)) {
                    return true;
                }
            }
            return false;
        }

        // FlowingFluid.spreadToSides plus getSpread: neighbouring cells are scored by their
        // distance to a drop, and only the closest ones are filled.
        void spread_to_sides(const BlockPos& pos, Kind kind, int level) {
            int amount = amount_of(level) - fluid(kind).drop_off;
            if (is_falling(level)) {
                // A landed column pushes level 1 outwards whatever its own amount was.
                amount = kFullAmount - 1;
            }
            if (amount <= 0) {
                return;
            }

            std::array<int, 4> distances{ -1, -1, -1, -1 };
            int lowest = kNoPath;
            for (int dir = 0; dir < 4; ++dir) {
                const BlockPos side = neighbour(pos, dir);
                if (!can_pass(side, kind)) {
                    continue;
                }
                if (new_level(side, kind) < 0) {
                    continue;
                }
                if (!can_replace_with(state_at(side), kind, false)) {
                    continue;
                }
                const int distance = is_hole(side, kind) ? 0 : slope_distance(side, dir, 1, kind);
                distances[static_cast<std::size_t>(dir)] = distance;
                if (distance < lowest) {
                    lowest = distance;
                }
            }

            for (int dir = 0; dir < 4; ++dir) {
                const int distance = distances[static_cast<std::size_t>(dir)];
                if (distance < 0) {
                    continue;
                }
                // A drop within reach wins over an even spread, which is why water runs to
                // the nearest hole instead of fanning out over a flat floor.
                if (lowest != kNoPath && distance != lowest) {
                    continue;
                }
                const BlockPos side = neighbour(pos, dir);
                const int next = new_level(side, kind);
                if (next < 0) {
                    continue;
                }
                if (solidify(side, kind)) {
                    continue;
                }
                write_fluid(side, kind, next);
            }
        }

        // FlowingFluid.spread: straight down first, sideways only where it can.
        void spread(const BlockPos& pos, Kind kind, int level) {
            const BlockPos down = below(pos);
            if (can_pass(down, kind) && can_hold(state_at(down))) {
                if (solidify(down, kind)) {
                    return;
                }
                const int next = new_level(down, kind);
                if (next >= 0 && can_replace_with(state_at(down), kind, true)) {
                    if (write_fluid(down, kind, next) && source_neighbours(pos, kind) >= 3) {
                        spread_to_sides(pos, kind, level);
                    }
                    return;
                }
            }

            // A flow whose below is a hole stays where it is and only fans out once it has
            // landed, while a source always spreads.
            if (level == 0 || !is_hole(pos, kind)) {
                spread_to_sides(pos, kind, level);
            }
        }

        void step(const BlockPos& pos) {
            if (world_ == nullptr || !inside(pos)) {
                return;
            }
            const Cell cell = cell_at(pos);
            if (cell.kind < 0) {
                return;
            }
            const auto kind = static_cast<Kind>(cell.kind);

            if (kind == Kind::Lava && lava_touches_water(pos)) {
                write_block(pos, cell.level == 0 ? obsidian_ : cobblestone_);
                return;
            }

            int level = cell.level;
            if (level != 0) {
                const int next = new_level(pos, kind);
                if (next < 0) {
                    write_block(pos, air_state_);
                    return;
                }
                if (next != level) {
                    write_fluid(pos, kind, next);
                    level = next;
                }
            }
            spread(pos, kind, level);
        }

        BlockWorld* world_ = nullptr;
        std::int32_t air_state_ = 0;
        std::int32_t min_y_ = -64;
        std::int32_t max_y_ = 319;
        std::array<Fluid, kKinds> fluids_;
        std::unordered_set<std::int32_t> replaceable_;
        std::int32_t obsidian_ = -1;
        std::int32_t cobblestone_ = -1;
        std::int32_t stone_ = -1;
        StateAt state_at_;
        ChangeSink sink_;
        std::deque<Job> queue_;
        std::unordered_set<std::uint64_t> queued_;
        // Cells written during the current tick, skipped so a single tick never cascades.
        std::unordered_set<std::uint64_t> touched_;
        std::vector<Job> batch_;
    };

} // namespace terila::world
