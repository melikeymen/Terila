#include "commands/registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace terila::commands {

    namespace {

        // The game refuses a fill larger than 32768 blocks, so a command cannot turn one
        // line into an unbounded amount of work.
        constexpr std::int64_t kMaxFillVolume = 32768;

        // 0 replace, 1 keep, 2 outline, 3 hollow.
        constexpr int kReplace = 0;
        constexpr int kKeep = 1;
        constexpr int kOutline = 2;
        constexpr int kHollow = 3;

        bool resolve_block_coords(const Position& position, const Origin& origin,
            std::int32_t& x, std::int32_t& y, std::int32_t& z) {
            double fx = 0.0;
            double fy = 0.0;
            double fz = 0.0;
            resolve_position(position, origin.x, origin.y, origin.z, origin.yaw, origin.pitch,
                fx, fy, fz);
            if (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(fz)) {
                return false;
            }
            x = static_cast<std::int32_t>(std::floor(fx));
            y = static_cast<std::int32_t>(std::floor(fy));
            z = static_cast<std::int32_t>(std::floor(fz));
            return true;
        }

        Result set_one_block(Exec& exec, int mode) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            const auto* position = exec.args.get<Position>("pos");
            const auto* block = exec.args.get<BlockStateValue>("block");
            if (position == nullptr || block == nullptr) {
                return Result{ 0, false };
            }

            std::int32_t x = 0;
            std::int32_t y = 0;
            std::int32_t z = 0;
            if (!resolve_block_coords(*position, origin, x, y, z)) {
                origin.say("The position is outside the world");
                return Result{ 0, false };
            }

            const std::int32_t existing = origin.game->block_state_at(x, y, z);
            if (existing < 0) {
                origin.say("The position is outside the world");
                return Result{ 0, false };
            }
            // `keep` only writes into air; the other form replaces whatever is there.
            if (mode == kKeep && existing != 0) {
                origin.say("Could not set the block");
                return Result{ 0, false };
            }

            if (!origin.game->set_block_state(x, y, z, block->id)) {
                origin.say("Could not set the block");
                return Result{ 0, false };
            }
            origin.say(std::format("Changed the block at ({}, {}, {})", x, y, z));
            return Result{ 1, true };
        }

        Result run_setblock(Exec& exec) { return set_one_block(exec, kReplace); }
        Result run_setblock_keep(Exec& exec) { return set_one_block(exec, kKeep); }

        Result run_fill(Exec& exec, int mode) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            const auto* from = exec.args.get<Position>("from");
            const auto* to = exec.args.get<Position>("to");
            const auto* block = exec.args.get<BlockStateValue>("block");
            if (from == nullptr || to == nullptr || block == nullptr) {
                return Result{ 0, false };
            }

            std::int32_t ax = 0;
            std::int32_t ay = 0;
            std::int32_t az = 0;
            std::int32_t bx = 0;
            std::int32_t by = 0;
            std::int32_t bz = 0;
            if (!resolve_block_coords(*from, origin, ax, ay, az) ||
                !resolve_block_coords(*to, origin, bx, by, bz)) {
                origin.say("The region is outside the world");
                return Result{ 0, false };
            }

            const std::int32_t min_x = std::min(ax, bx);
            const std::int32_t min_y = std::min(ay, by);
            const std::int32_t min_z = std::min(az, bz);
            const std::int32_t max_x = std::max(ax, bx);
            const std::int32_t max_y = std::max(ay, by);
            const std::int32_t max_z = std::max(az, bz);

            const std::int64_t width = static_cast<std::int64_t>(max_x) - min_x + 1;
            const std::int64_t height = static_cast<std::int64_t>(max_y) - min_y + 1;
            const std::int64_t depth = static_cast<std::int64_t>(max_z) - min_z + 1;
            if (width * height * depth > kMaxFillVolume) {
                origin.say(std::format("The region is too large ({} blocks, the limit is {})",
                    width * height * depth, kMaxFillVolume));
                return Result{ 0, false };
            }

            std::int32_t filled = 0;
            for (std::int32_t x = min_x; x <= max_x; ++x) {
                for (std::int32_t y = min_y; y <= max_y; ++y) {
                    for (std::int32_t z = min_z; z <= max_z; ++z) {
                        const bool on_shell = x == min_x || x == max_x || y == min_y ||
                            y == max_y || z == min_z || z == max_z;

                        std::int32_t state = block->id;
                        if (mode == kOutline || mode == kHollow) {
                            if (!on_shell) {
                                if (mode == kOutline) {
                                    continue;
                                }
                                state = 0;   // hollow: the inside becomes air
                            }
                        }

                        const std::int32_t existing = origin.game->block_state_at(x, y, z);
                        if (existing < 0 || existing == state) {
                            continue;
                        }
                        if (mode == kKeep && existing != 0) {
                            continue;
                        }
                        if (!origin.game->set_block_state(x, y, z, state)) {
                            continue;
                        }
                        ++filled;
                    }
                }
            }

            if (filled == 0) {
                origin.say("No blocks were filled");
                return Result{ 0, false };
            }
            origin.say(std::format("Successfully filled {} block{}", filled,
                filled == 1 ? "" : "s"));
            return Result{ filled, true };
        }

        Result run_fill_replace(Exec& exec) { return run_fill(exec, kReplace); }
        Result run_fill_keep(Exec& exec) { return run_fill(exec, kKeep); }
        Result run_fill_outline(Exec& exec) { return run_fill(exec, kOutline); }
        Result run_fill_hollow(Exec& exec) { return run_fill(exec, kHollow); }

    } // namespace

    void install_edit(Registry& registry) {
        Node& setblock = registry.root().literal("setblock", Capability::Operator);
        Node& placed = setblock.argument("pos", Parser::position());
        Node& state = placed.argument("block", registry.block_state_parser());
        state.executes(run_setblock);
        state.literal("replace").executes(run_setblock);
        state.literal("keep").executes(run_setblock_keep);

        Node& fill = registry.root().literal("fill", Capability::Operator);
        Node& corners = fill.argument("from", Parser::position())
            .argument("to", Parser::position());
        Node& filled = corners.argument("block", registry.block_state_parser());
        filled.executes(run_fill_replace);
        filled.literal("replace").executes(run_fill_replace);
        filled.literal("keep").executes(run_fill_keep);
        filled.literal("outline").executes(run_fill_outline);
        filled.literal("hollow").executes(run_fill_hollow);
    }

} // namespace terila::commands
