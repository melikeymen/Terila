#pragma once

// Lighting a nether portal. A player frames an opening in obsidian and uses flint and steel
// inside it; the opening may be 2..21 blocks wide and 3..21 tall, and lighting it fills the
// inside with portal blocks.
//
// The search walks down to the row that stands on obsidian, widens to the left corner, then
// measures the opening and checks the obsidian ring around it. Both plane axes are tried,
// because a player usually clicks the top of the bottom frame block and the facing is not
// part of that packet.

#include "world/block_world.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace terila::world {

    inline constexpr int kPortalMinWidth = 2;
    inline constexpr int kPortalMaxWidth = 21;
    inline constexpr int kPortalMinHeight = 3;
    inline constexpr int kPortalMaxHeight = 21;

    // The empty opening of a finished frame, anchored at its bottom-left corner.
    struct PortalFrame {
        BlockPos bottom_left{};
        int width = 0;
        int height = 0;
        bool along_x = false;

        // Every cell the portal blocks go into.
        std::vector<BlockPos> interior() const {
            const int sx = along_x ? 1 : 0;
            const int sz = along_x ? 0 : 1;
            std::vector<BlockPos> cells;
            cells.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
            for (int row = 0; row < height; ++row) {
                for (int column = 0; column < width; ++column) {
                    cells.push_back(BlockPos{ bottom_left.x + sx * column,
                        bottom_left.y + row, bottom_left.z + sz * column });
                }
            }
            return cells;
        }
    };

    namespace detail {

        template<class StateAt, class IsFrame, class IsInterior>
        std::optional<PortalFrame> search_plane(const BlockPos& inside, bool along_x,
            int world_bottom, int world_top, StateAt state_at, IsFrame is_frame,
            IsInterior is_interior) {
            const auto at = [&](int x, int y, int z) {
                return state_at(BlockPos{ x, y, z });
            };

            auto corner = inside;
            if (!is_interior(at(corner.x, corner.y, corner.z))) {
                return std::nullopt;
            }

            // Down to the row that stands on the frame. A column taller than the tallest
            // portal cannot be the inside of one, so the walk stops there.
            for (int step = 0; step < kPortalMaxHeight; ++step) {
                if (is_frame(at(corner.x, corner.y - 1, corner.z))) {
                    break;
                }
                if (corner.y - 1 < world_bottom) {
                    return std::nullopt;
                }
                corner.y -= 1;
                if (!is_interior(at(corner.x, corner.y, corner.z))) {
                    return std::nullopt;
                }
            }
            if (!is_frame(at(corner.x, corner.y - 1, corner.z))) {
                return std::nullopt;
            }

            const int sx = along_x ? 1 : 0;
            const int sz = along_x ? 0 : 1;

            // Left corner: the opening ends where the next cell is not empty, and that cell
            // has to be the frame's side.
            for (int step = 0; step < kPortalMaxWidth; ++step) {
                if (!is_interior(at(corner.x - sx, corner.y, corner.z - sz))) {
                    break;
                }
                if (!is_frame(at(corner.x - sx, corner.y - 1, corner.z - sz))) {
                    return std::nullopt;
                }
                corner.x -= sx;
                corner.z -= sz;
            }
            if (!is_frame(at(corner.x - sx, corner.y, corner.z - sz))) {
                return std::nullopt;
            }

            // Width of the bottom row, which has to sit on the frame the whole way.
            int width = 0;
            while (width < kPortalMaxWidth) {
                const int x = corner.x + sx * width;
                const int z = corner.z + sz * width;
                if (!is_interior(at(x, corner.y, z)) ||
                    !is_frame(at(x, corner.y - 1, z))) {
                    break;
                }
                ++width;
            }
            if (width < kPortalMinWidth ||
                !is_frame(at(corner.x + sx * width, corner.y, corner.z + sz * width))) {
                return std::nullopt;
            }

            // Height of the opening: every row has to be empty across the full width.
            int height = 0;
            while (height < kPortalMaxHeight) {
                const int y = corner.y + height;
                if (y > world_top) {
                    break;
                }
                bool full = true;
                for (int column = 0; column < width; ++column) {
                    if (!is_interior(at(corner.x + sx * column, y, corner.z + sz * column))) {
                        full = false;
                        break;
                    }
                }
                if (!full) {
                    break;
                }
                ++height;
            }
            if (height < kPortalMinHeight || corner.y + height > world_top) {
                return std::nullopt;
            }

            // The ring around the opening: the top row and the two side columns, corners
            // included, are the frame the player built.
            const int top = corner.y + height;
            for (int column = -1; column <= width; ++column) {
                if (!is_frame(at(corner.x + sx * column, top, corner.z + sz * column))) {
                    return std::nullopt;
                }
            }
            for (int row = 0; row < height; ++row) {
                const int y = corner.y + row;
                if (!is_frame(at(corner.x - sx, y, corner.z - sz)) ||
                    !is_frame(at(corner.x + sx * width, y, corner.z + sz * width))) {
                    return std::nullopt;
                }
            }
            for (int column = -1; column <= width; ++column) {
                if (!is_frame(at(corner.x + sx * column, corner.y - 1,
                        corner.z + sz * column))) {
                    return std::nullopt;
                }
            }

            PortalFrame frame;
            frame.bottom_left = corner;
            frame.width = width;
            frame.height = height;
            frame.along_x = along_x;
            return frame;
        }

    } // namespace detail

    // The frame an opening belongs to, tried along x and then along z. `inside` is the cell
    // the fire was lit in; a cell anywhere inside the opening finds the same frame.
    template<class StateAt, class IsFrame, class IsInterior>
    std::optional<PortalFrame> find_portal_frame(const BlockPos& inside, int world_bottom,
        int world_top, StateAt state_at, IsFrame is_frame, IsInterior is_interior) {
        if (auto along_x = detail::search_plane(inside, true, world_bottom, world_top,
                state_at, is_frame, is_interior)) {
            return along_x;
        }
        return detail::search_plane(inside, false, world_bottom, world_top, state_at,
            is_frame, is_interior);
    }

} // namespace terila::world
