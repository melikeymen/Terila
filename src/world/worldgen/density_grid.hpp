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

    struct DensityGrid {
        static constexpr int kCellsX = 5;
        static constexpr int kCellsZ = 5;
        int y_count = 0;
        int origin_x = 0;
        int origin_z = 0;
        int origin_y = 0;
        std::vector<double> values;

        void init(std::int32_t wx0, std::int32_t wz0, int min_y, int y_count_in) {
            origin_x = wx0;
            origin_z = wz0;
            origin_y = min_y;
            y_count = y_count_in;
            values.assign(static_cast<std::size_t>(kCellsX * kCellsZ * y_count), 0.0);
        }

        std::size_t index(int ix, int iy, int iz) const {
            return (static_cast<std::size_t>(iy) * kCellsZ + static_cast<std::size_t>(iz)) *
                kCellsX + static_cast<std::size_t>(ix);
        }

        double at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            const int ix = std::clamp((x - origin_x) >> 2, 0, kCellsX - 2);
            const int iz = std::clamp((z - origin_z) >> 2, 0, kCellsZ - 2);
            const int iy = std::clamp((y - origin_y) >> 3, 0, y_count - 2);
            const double tx = static_cast<double>(x - (origin_x + ix * 4)) / 4.0;
            const double tz = static_cast<double>(z - (origin_z + iz * 4)) / 4.0;
            const double ty = static_cast<double>(y - (origin_y + iy * 8)) / 8.0;

            const double c000 = values[index(ix, iy, iz)];
            const double c100 = values[index(ix + 1, iy, iz)];
            const double c010 = values[index(ix, iy, iz + 1)];
            const double c110 = values[index(ix + 1, iy, iz + 1)];
            const double c001 = values[index(ix, iy + 1, iz)];
            const double c101 = values[index(ix + 1, iy + 1, iz)];
            const double c011 = values[index(ix, iy + 1, iz + 1)];
            const double c111 = values[index(ix + 1, iy + 1, iz + 1)];

            const double bottom = (c000 + (c100 - c000) * tx) +
                ((c010 + (c110 - c010) * tx) - (c000 + (c100 - c000) * tx)) * tz;
            const double top = (c001 + (c101 - c001) * tx) +
                ((c011 + (c111 - c011) * tx) - (c001 + (c101 - c001) * tx)) * tz;
            return bottom + (top - bottom) * ty;
        }
    };

} // namespace terila::world::worldgen
