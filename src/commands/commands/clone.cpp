#include "commands/registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        // The game's own ceiling for one command's block modifications.
        constexpr std::int64_t kMaxCloneVolume = 32768;

        constexpr int kReplace = 0;
        constexpr int kMasked = 1;
        constexpr int kFiltered = 2;

        // normal refuses to write over its own source, force overwrites and move also
        // empties the source afterwards.
        constexpr int kNormal = 0;
        constexpr int kForce = 1;
        constexpr int kMove = 2;

        struct Cell {
            std::int32_t source_x = 0;
            std::int32_t source_y = 0;
            std::int32_t source_z = 0;
            std::int32_t target_x = 0;
            std::int32_t target_y = 0;
            std::int32_t target_z = 0;
            std::int32_t state = 0;
        };

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

        Result run_clone(Exec& exec, int block_mode, int placement) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            const auto* begin = exec.args.get<Position>("begin");
            const auto* end = exec.args.get<Position>("end");
            const auto* destination = exec.args.get<Position>("destination");
            if (begin == nullptr || end == nullptr || destination == nullptr) {
                return Result{ 0, false };
            }

            std::int32_t ax = 0;
            std::int32_t ay = 0;
            std::int32_t az = 0;
            std::int32_t bx = 0;
            std::int32_t by = 0;
            std::int32_t bz = 0;
            std::int32_t dx = 0;
            std::int32_t dy = 0;
            std::int32_t dz = 0;
            if (!resolve_block_coords(*begin, origin, ax, ay, az) ||
                !resolve_block_coords(*end, origin, bx, by, bz) ||
                !resolve_block_coords(*destination, origin, dx, dy, dz)) {
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
            const std::int64_t volume = width * height * depth;
            if (volume > kMaxCloneVolume) {
                origin.say(std::format("The region is too large ({} blocks, the limit is {})",
                    volume, kMaxCloneVolume));
                return Result{ 0, false };
            }

            // The destination occupies the same shape one corner further along, so an
            // overlap is decided before a single block is read.
            const bool overlaps = !(dx + width - 1 < min_x || dx > max_x ||
                dy + height - 1 < min_y || dy > max_y ||
                dz + depth - 1 < min_z || dz > max_z);
            if (placement == kNormal && overlaps) {
                origin.say("Source and destination overlap");
                return Result{ 0, false };
            }

            std::int32_t filter_state = -1;
            if (block_mode == kFiltered) {
                const auto* filter = exec.args.get<BlockStateValue>("filter");
                if (filter == nullptr) {
                    return Result{ 0, false };
                }
                filter_state = filter->id;
            }

            std::vector<Cell> cells;
            cells.reserve(static_cast<std::size_t>(volume));
            for (std::int32_t y = min_y; y <= max_y; ++y) {
                for (std::int32_t z = min_z; z <= max_z; ++z) {
                    for (std::int32_t x = min_x; x <= max_x; ++x) {
                        const std::int32_t state = origin.game->block_state_at(x, y, z);
                        if (state < 0) {
                            continue;
                        }
                        if (block_mode == kMasked && state == 0) {
                            continue;
                        }
                        if (block_mode == kFiltered && state != filter_state) {
                            continue;
                        }
                        cells.push_back(Cell{ x, y, z, x + (dx - min_x), y + (dy - min_y),
                            z + (dz - min_z), state });
                    }
                }
            }

            if (cells.empty()) {
                origin.say("No blocks were cloned");
                return Result{ 0, false };
            }

            // A move empties its source first, so a copy that overlaps its own source
            // cannot erase what was just written.
            if (placement == kMove) {
                for (const auto& cell : cells) {
                    origin.game->set_block_state(cell.source_x, cell.source_y,
                        cell.source_z, 0);
                }
            }

            std::int32_t cloned = 0;
            for (const auto& cell : cells) {
                if (!origin.game->set_block_state(cell.target_x, cell.target_y, cell.target_z,
                        cell.state)) {
                    continue;
                }
                ++cloned;
            }

            if (cloned == 0) {
                origin.say("No blocks were cloned");
                return Result{ 0, false };
            }
            origin.say(std::format("Successfully cloned {} block{}", cloned,
                cloned == 1 ? "" : "s"));
            return Result{ cloned, true };
        }

    } // namespace

    void install_clone(Registry& registry) {
        Node& clone = registry.root().literal("clone", Capability::Operator);
        Node& destination = clone.argument("begin", Parser::position())
            .argument("end", Parser::position())
            .argument("destination", Parser::position());

        // Every block filter accepts the same three placement modes, and the bare form is
        // `replace` in `normal` placement.
        const auto add_placements = [](Node& node, int block_mode) {
            node.executes([block_mode](Exec& exec) {
                return run_clone(exec, block_mode, kNormal);
            });
            node.literal("force").executes([block_mode](Exec& exec) {
                return run_clone(exec, block_mode, kForce);
            });
            node.literal("move").executes([block_mode](Exec& exec) {
                return run_clone(exec, block_mode, kMove);
            });
            node.literal("normal").executes([block_mode](Exec& exec) {
                return run_clone(exec, block_mode, kNormal);
            });
        };

        add_placements(destination, kReplace);

        Node& replace = destination.literal("replace");
        add_placements(replace, kReplace);

        Node& masked = destination.literal("masked");
        add_placements(masked, kMasked);

        Node& filtered = destination.literal("filtered")
            .argument("filter", registry.block_state_parser());
        add_placements(filtered, kFiltered);
    }

} // namespace terila::commands
