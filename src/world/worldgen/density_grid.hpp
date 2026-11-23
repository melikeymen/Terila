#pragma once

// The density field of one chunk on the grid the game interpolates over: four blocks
// horizontally and eight vertically. Sampling the corners once and blending them is what
// keeps a chunk's 98k blocks affordable, and it is also the baseline every single block read
// agrees with.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace terila::world::worldgen {

    // One interpolation for the whole field. The chunk's grid and a single block read sample
    // the same eight corners of the same 4x8 cell, so both must blend them with the same
    // arithmetic: two copies of the formula can disagree in the last bit, and a value that
    // sits on zero would then be air to one path and stone to the other.
    inline double blend_cell(const double corners[8], double tx, double ty, double tz) {
        const double c000 = corners[0];
        const double c100 = corners[1];
        const double c010 = corners[2];
        const double c110 = corners[3];
        const double c001 = corners[4];
        const double c101 = corners[5];
        const double c011 = corners[6];
        const double c111 = corners[7];
        const double bottom = (c000 + (c100 - c000) * tx) +
            ((c010 + (c110 - c010) * tx) - (c000 + (c100 - c000) * tx)) * tz;
        const double top = (c001 + (c101 - c001) * tx) +
            ((c011 + (c111 - c011) * tx) - (c001 + (c101 - c001) * tx)) * tz;
        return bottom + (top - bottom) * ty;
    }

    struct DensityGrid {
        // A chunk's field is five by five cells so that its sixteen blocks are covered; a grid
        // with a margin carries more and serves the neighbours of a chunk as well.
        int cells_x = 5;
        int cells_z = 5;
        int y_count = 0;
        int origin_x = 0;
        int origin_z = 0;
        int origin_y = 0;
        std::vector<double> values;

        void init(std::int32_t wx0, std::int32_t wz0, int min_y, int y_count_in,
            int cells_x_in = 5, int cells_z_in = 5) {
            origin_x = wx0;
            origin_z = wz0;
            origin_y = min_y;
            y_count = y_count_in;
            cells_x = cells_x_in;
            cells_z = cells_z_in;
            values.assign(static_cast<std::size_t>(cells_x * cells_z * y_count), 0.0);
        }

        std::size_t index(int ix, int iy, int iz) const {
            return (static_cast<std::size_t>(iy) * static_cast<std::size_t>(cells_z) +
                static_cast<std::size_t>(iz)) * static_cast<std::size_t>(cells_x) +
                static_cast<std::size_t>(ix);
        }

        double at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            const int ix = std::clamp((x - origin_x) >> 2, 0, cells_x - 2);
            const int iz = std::clamp((z - origin_z) >> 2, 0, cells_z - 2);
            const int iy = std::clamp((y - origin_y) >> 3, 0, y_count - 2);
            const double tx = static_cast<double>(x - (origin_x + ix * 4)) / 4.0;
            const double tz = static_cast<double>(z - (origin_z + iz * 4)) / 4.0;
            const double ty = static_cast<double>(y - (origin_y + iy * 8)) / 8.0;

            const double corners[8] = {
                values[index(ix, iy, iz)],
                values[index(ix + 1, iy, iz)],
                values[index(ix, iy, iz + 1)],
                values[index(ix + 1, iy, iz + 1)],
                values[index(ix, iy + 1, iz)],
                values[index(ix + 1, iy + 1, iz)],
                values[index(ix, iy + 1, iz + 1)],
                values[index(ix + 1, iy + 1, iz + 1)],
            };
            return blend_cell(corners, tx, ty, tz);
        }
    };

} // namespace terila::world::worldgen
