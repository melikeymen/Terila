#pragma once

// The fluids inside the rock. Height alone does not decide where water stands: the game
// scatters aquifer cells over a coarse lattice, gives every cell a random centre and a fluid
// level of its own, and a block the density leaves open takes the level of the cells nearest
// to it, blended so two bodies meet without a wall of water between them. A barrier noise
// pushes rock back over some of those bodies, which is what ends a cave in stone instead of
// pouring it into the one next door. The same construction lives here, driven by the functions
// the preset names.

#include "world/worldgen/density/density.hpp"
#include "world/worldgen/density/noise.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace terila::world::worldgen {

    // What the density field leaves in a block once the fluids had their say.
    enum class Substance : std::uint8_t { Solid, Air, Water, Lava };

    // The six fields a preset names for its aquifer. A preset without them (a superflat) keeps
    // the plain sea level rule.
    struct AquiferFunctions {
        DfPtr barrier;
        DfPtr floodedness;
        DfPtr spread;
        DfPtr lava;
        DfPtr exclusion;
        DfPtr surface;

        bool ready() const {
            return barrier && floodedness && spread && lava && exclusion && surface;
        }
    };

    // One fluid body: everything under its level holds its fluid, everything above is air.
    struct FluidStatus {
        int level = 0;
        Substance type = Substance::Air;

        Substance at(int y) const { return y < level ? type : Substance::Air; }

        bool operator==(const FluidStatus& other) const {
            return level == other.level && type == other.type;
        }
    };

    class Aquifer {
    public:
        // A level no block ever reaches, used for "this cell holds nothing".
        static constexpr int kWayBelow = -1000000;

        void init(const DensityGraph* graph, const AquiferFunctions& functions,
            std::uint32_t seed, int cache_slots, int min_y, int max_y, FluidStatus bubble) {
            graph_ = graph;
            functions_ = functions;
            min_y_ = min_y;
            max_y_ = max_y;
            bubble_ = bubble;
            factory_ = RandomSource::from_seed(seed).fork_positional();
            context_.reset(cache_slots);
            ready_ = graph_ != nullptr && functions_.ready();
        }

        bool ready() const { return ready_; }

        // The lattice of one chunk, sized from the volume about to be walked: every cell a
        // block inside it can be nearest to, which is the cell itself plus one row around.
        void begin_chunk(int block_x0, int block_z0) {
            if (!ready_) {
                return;
            }
            min_gx_ = grid_x(block_x0 - 5);
            max_gx_ = grid_x(block_x0 + 15 - 5) + 1;
            min_gz_ = grid_z(block_z0 - 5);
            max_gz_ = grid_z(block_z0 + 15 - 5) + 1;
            min_gy_ = grid_y(min_y_ + 1) - 1;
            max_gy_ = grid_y(max_y_ + 1) + 1;
            size_x_ = max_gx_ - min_gx_ + 1;
            size_y_ = max_gy_ - min_gy_ + 1;
            size_z_ = max_gz_ - min_gz_ + 1;
            const auto cells = static_cast<std::size_t>(size_x_ * size_y_ * size_z_);
            points_.assign(cells, Point{});
            statuses_.assign(cells, FluidStatus{});
            known_.assign(cells, 0);
            surfaces_.clear();

            // Above the highest surface of the chunk nothing can be wetter than the dimension's
            // own sea, so the cell walk starts there and the ocean above is answered at once.
            int surface_max = kWayBelow;
            for (int dz = 0; dz <= 15; dz += 5) {
                for (int dx = 0; dx <= 15; dx += 5) {
                    surface_max = std::max(surface_max, surface_level(block_x0 + dx,
                        block_z0 + dz));
                }
            }
            skip_above_ = std::min(max_y_, surface_max + 8 + 12);
        }

        // What one block holds. A block the density already fills is rock before any fluid is
        // asked, which is also the short path that keeps the deep bulk cheap.
        Substance substance(int x, int y, int z, double density) {
            if (density > 0.0) {
                return Substance::Solid;
            }
            // A preset without an aquifer config (or a chunk that never began) keeps the plain
            // sea: everything under the dimension's fluid level is that fluid.
            if (!ready_) {
                return bubble_.at(y);
            }
            if (y > skip_above_) {
                return bubble_.at(y);
            }
            if (bubble_.at(y) == Substance::Lava) {
                return Substance::Lava;
            }

            const int anchor_x = grid_x(x - 5);
            const int anchor_y = grid_y(y + 1);
            const int anchor_z = grid_z(z - 5);
            int distance[4] = { kFar, kFar, kFar, kFar };
            int nearest[4] = { 0, 0, 0, 0 };
            for (int dx = 0; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = 0; dz <= 1; ++dz) {
                        const int gx = anchor_x + dx;
                        const int gy = anchor_y + dy;
                        const int gz = anchor_z + dz;
                        const int cell = index_of(gx, gy, gz);
                        if (cell < 0) {
                            continue;
                        }
                        const Point& point = point_of(cell, gx, gy, gz);
                        const int off_x = point.x - x;
                        const int off_y = point.y - y;
                        const int off_z = point.z - z;
                        const int away = off_x * off_x + off_y * off_y + off_z * off_z;
                        for (int slot = 0; slot < 4; ++slot) {
                            if (away <= distance[slot]) {
                                for (int move = 3; move > slot; --move) {
                                    distance[move] = distance[move - 1];
                                    nearest[move] = nearest[move - 1];
                                }
                                distance[slot] = away;
                                nearest[slot] = cell;
                                break;
                            }
                        }
                    }
                }
            }

            const FluidStatus first = status_of(nearest[0]);
            const double similar12 = similarity(distance[0], distance[1]);
            const Substance first_at = first.at(y);
            if (similar12 <= 0.0) {
                return first_at;
            }
            // Water sitting on top of a lava sea stays water; the two never merge.
            if (first_at == Substance::Water && bubble_.at(y - 1) == Substance::Lava) {
                return first_at;
            }

            const FluidStatus second = status_of(nearest[1]);
            if (density + similar12 * pressure(x, y, z, first, second) > 0.0) {
                return Substance::Solid;
            }
            const FluidStatus third = status_of(nearest[2]);
            const double similar13 = similarity(distance[0], distance[2]);
            if (similar13 > 0.0 &&
                density + similar12 * similar13 * pressure(x, y, z, first, third) > 0.0) {
                return Substance::Solid;
            }
            const double similar23 = similarity(distance[1], distance[2]);
            if (similar23 > 0.0 &&
                density + similar12 * similar23 * pressure(x, y, z, second, third) > 0.0) {
                return Substance::Solid;
            }
            return first_at;
        }

    private:
        struct Point {
            int x = 0;
            int y = 0;
            int z = 0;
            bool valid = false;
        };

        static constexpr int kFar = std::numeric_limits<int>::max();
        // How far apart two cell centres can be and still be blended: twenty five blocks
        // squared, the distance the game measures its similarity against.
        static constexpr double kBlendRadiusSquared = 25.0;

        static int grid_x(int block) { return block >> 4; }
        static int grid_z(int block) { return block >> 4; }
        static int grid_y(int block) {
            return block >= 0 ? block / 12 : -((-block + 11) / 12);
        }
        static int from_x(int grid, int offset) { return (grid << 4) + offset; }
        static int from_y(int grid, int offset) { return grid * 12 + offset; }
        static int from_z(int grid, int offset) { return (grid << 4) + offset; }

        int index_of(int gx, int gy, int gz) const {
            if (gx < min_gx_ || gx > max_gx_ || gy < min_gy_ || gy > max_gy_ ||
                gz < min_gz_ || gz > max_gz_) {
                return -1;
            }
            return ((gy - min_gy_) * size_z_ + (gz - min_gz_)) * size_x_ + (gx - min_gx_);
        }

        // Where a cell's body sits inside it: the cell is sixteen wide and twelve tall, and the
        // centre is a position the cell's own hash picks, so two neighbouring cells never share
        // one and the pattern repeats only with the world.
        const Point& point_of(int cell, int gx, int gy, int gz) {
            Point& point = points_[static_cast<std::size_t>(cell)];
            if (point.valid) {
                return point;
            }
            RandomSource random = factory_.at(gx, gy, gz);
            point.x = from_x(gx, random.next_int(10));
            point.y = from_y(gy, random.next_int(9));
            point.z = from_z(gz, random.next_int(10));
            point.valid = true;
            return point;
        }

        static double similarity(int close, int far) {
            return 1.0 - static_cast<double>(far - close) / kBlendRadiusSquared;
        }

        // How hard the rock pushes back where two bodies meet: the further the block is from
        // the shared fluid level, the less pressure, and the barrier noise breaks up the rest.
        double pressure(int x, int y, int z, const FluidStatus& left, const FluidStatus& right) {
            const Substance left_at = left.at(y);
            const Substance right_at = right.at(y);
            if ((left_at == Substance::Lava && right_at == Substance::Water) ||
                (left_at == Substance::Water && right_at == Substance::Lava)) {
                return 2.0;
            }
            const int gap = std::abs(left.level - right.level);
            if (gap == 0) {
                return 0.0;
            }
            const double middle = 0.5 * static_cast<double>(left.level + right.level);
            const double above = static_cast<double>(y) + 0.5 - middle;
            const double half = static_cast<double>(gap) / 2.0;
            const double from_edge = half - std::abs(above);
            double gradient = 0.0;
            if (above > 0.0) {
                gradient = from_edge > 0.0 ? from_edge / 1.5 : from_edge / 2.5;
            } else {
                const double centre = 3.0 + from_edge;
                gradient = centre > 0.0 ? centre / 3.0 : centre / 10.0;
            }
            double noise = 0.0;
            if (gradient >= -2.0 && gradient <= 2.0) {
                noise = sample(functions_.barrier, x, y, z);
            }
            return 2.0 * (noise + gradient);
        }

        const FluidStatus& status_of(int cell) {
            const auto slot = static_cast<std::size_t>(cell);
            if (known_[slot] == 0) {
                const Point& point = points_[slot];
                statuses_[slot] = fluid_at_point(point.x, point.y, point.z);
                known_[slot] = 1;
            }
            return statuses_[slot];
        }

        // The surface the aquifer treats as ground: the same estimate the terrain uses, asked at
        // the four block lattice the game quantises it to.
        int surface_level(int x, int z) {
            const int qx = (x >> 2) << 2;
            const int qz = (z >> 2) << 2;
            const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(qx))
                << 32) | static_cast<std::uint32_t>(qz);
            const auto found = surfaces_.find(key);
            if (found != surfaces_.end()) {
                return found->second;
            }
            const int level = static_cast<int>(std::floor(sample(functions_.surface, qx, 0, qz)));
            surfaces_[key] = level;
            return level;
        }

        static constexpr int kSurfaceAdjust = 8;
        static constexpr int kSurfaceSamples = 13;
        static constexpr int kSurfaceOffsets[kSurfaceSamples][2] = {
            { 0, 0 }, { -2, -1 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { -3, 0 }, { -2, 0 },
            { -1, 0 }, { 1, 0 }, { -2, 1 }, { -1, 1 }, { 0, 1 }, { 1, 1 },
        };

        // The body a cell holds. A cell under a surface that the sea covers tends to be full,
        // one inside a mountain to be dry, and the floodedness noise decides the rest.
        FluidStatus fluid_at_point(int x, int y, int z) {
            int lowest_surface = std::numeric_limits<int>::max();
            const int top = y + 12;
            const int bottom = y - 12;
            bool centre_under_fluid = false;
            for (int index = 0; index < kSurfaceSamples; ++index) {
                const int sample_x = x + kSurfaceOffsets[index][0] * 16;
                const int sample_z = z + kSurfaceOffsets[index][1] * 16;
                const int surface = surface_level(sample_x, sample_z);
                const int adjusted = surface + kSurfaceAdjust;
                const bool centre = kSurfaceOffsets[index][0] == 0 && kSurfaceOffsets[index][1] == 0;
                if (centre && bottom > adjusted) {
                    return bubble_;
                }
                const bool reaches = top > adjusted;
                if (reaches || centre) {
                    const Substance above_surface = bubble_.at(adjusted);
                    if (above_surface != Substance::Air) {
                        if (centre) {
                            centre_under_fluid = true;
                        }
                        if (reaches) {
                            return bubble_;
                        }
                    }
                }
                lowest_surface = std::min(lowest_surface, surface);
            }
            const int level = body_level(x, y, z, lowest_surface, centre_under_fluid);
            return FluidStatus{ level, body_type(x, y, z, level) };
        }

        static double map(double value, double from_low, double from_high, double to_low,
            double to_high) {
            return to_low + (value - from_low) * (to_high - to_low) / (from_high - from_low);
        }

        int body_level(int x, int y, int z, int lowest_surface, bool centre_under_fluid) {
            if (sample(functions_.exclusion, x, y, z) > 0.0) {
                return kWayBelow;
            }
            const int below_surface = lowest_surface + kSurfaceAdjust - y;
            const double near_surface = centre_under_fluid
                ? std::clamp(map(static_cast<double>(below_surface), 0.0, 64.0, 1.0, 0.0),
                    0.0, 1.0)
                : 0.0;
            const double wet = std::clamp(sample(functions_.floodedness, x, y, z), -1.0, 1.0);
            const double full_edge = map(near_surface, 1.0, 0.0, -0.3, 0.8);
            const double part_edge = map(near_surface, 1.0, 0.0, -0.8, 0.4);
            if (wet - full_edge > 0.0) {
                return bubble_.level;
            }
            if (wet - part_edge > 0.0) {
                return spread_level(x, y, z, lowest_surface);
            }
            return kWayBelow;
        }

        // A body that is only part full stands at its own height inside a forty block band,
        // nudged by the spread noise so the surface is not one flat sheet.
        int spread_level(int x, int y, int z, int lowest_surface) {
            const int band_x = x >= 0 ? x / 16 : -((-x + 15) / 16);
            const int band_y = y >= 0 ? y / 40 : -((-y + 39) / 40);
            const int band_z = z >= 0 ? z / 16 : -((-z + 15) / 16);
            const int middle = band_y * 40 + 20;
            const double spread = sample(functions_.spread, band_x, band_y, band_z) * 10.0;
            const int quantized = static_cast<int>(std::floor(spread / 3.0)) * 3;
            return std::min(lowest_surface, middle + quantized);
        }

        // A body far below the sea is lava when the lava noise leans far enough either way.
        Substance body_type(int x, int y, int z, int level) {
            Substance type = bubble_.type;
            if (level <= -10 && level != kWayBelow && type != Substance::Lava) {
                const int band_x = x >= 0 ? x / 64 : -((-x + 63) / 64);
                const int band_y = y >= 0 ? y / 40 : -((-y + 39) / 40);
                const int band_z = z >= 0 ? z / 64 : -((-z + 63) / 64);
                if (std::abs(sample(functions_.lava, band_x, band_y, band_z)) > 0.3) {
                    type = Substance::Lava;
                }
            }
            return type;
        }

        double sample(const DfPtr& function, int x, int y, int z) {
            if (!function || graph_ == nullptr) {
                return 0.0;
            }
            return graph_->sample(function, static_cast<double>(x), static_cast<double>(y),
                static_cast<double>(z), context_);
        }

        const DensityGraph* graph_ = nullptr;
        AquiferFunctions functions_;
        EvalContext context_;
        PositionalFactory factory_;
        FluidStatus bubble_;
        int min_y_ = -64;
        int max_y_ = 320;
        bool ready_ = false;

        int min_gx_ = 0;
        int max_gx_ = 0;
        int min_gy_ = 0;
        int max_gy_ = 0;
        int min_gz_ = 0;
        int max_gz_ = 0;
        int size_x_ = 0;
        int size_y_ = 0;
        int size_z_ = 0;
        int skip_above_ = 0;
        std::vector<Point> points_;
        std::vector<FluidStatus> statuses_;
        std::vector<char> known_;
        std::unordered_map<std::uint64_t, int> surfaces_;
    };

} // namespace terila::world::worldgen
