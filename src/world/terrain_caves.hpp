#pragma once

// The two things that are carved into the noise terrain: caves, and the ore veins that sit
// under the surface. Both are pure functions of position, so a single block lookup and a
// whole chunk always agree.

#include "world/terrain_noise.hpp"
#include "world/terrain_biomes.hpp"

#include <cstdint>
#include <cstdlib>

namespace terila::world::terrain {

    // Caves: a big "cheese" field that hollows out caverns where it crosses zero, and two
    // tunnel fields whose ridges meet in thin passages. Neither is asked about a block close
    // to the surface, so the ground above never turns into a floating shell.
    class CaveCarver {
    public:
        void init(std::uint64_t seed, int min_y) {
            min_y_ = min_y;
            // A single octave per channel: caves are smooth enough that the extra detail is
            // invisible, and this is the hottest sampler in chunk generation.
            cheese_.init(seed ^ 0xC4E5Eull, 1, 1.0f / 58.0f);
            tunnel_a_.init(seed ^ 0x71A5ull, 1, 1.0f / 26.0f);
            tunnel_b_.init(seed ^ 0xB0B1ull, 1, 1.0f / 26.0f);
        }

        bool is_cave(std::int32_t x, std::int32_t y, std::int32_t z, int surface) const {
            if (y <= min_y_ + 2 || y > surface - 5) {
                return false;
            }
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            const float fz = static_cast<float>(z);

            const float cheese = cheese_.sample(fx, fy * 1.7f, fz);
            if (std::abs(cheese) < 0.055f) {
                return true;
            }

            const float a = tunnel_a_.sample(fx, fy * 1.8f, fz);
            const float b = tunnel_b_.sample(fx, fy * 1.8f, fz);
            return a * a + b * b < 0.0016f;
        }

    private:
        detail::OctaveNoise cheese_;
        detail::OctaveNoise tunnel_a_;
        detail::OctaveNoise tunnel_b_;
        int min_y_ = -64;
    };

    // Ore veins: each 8x8x8 cell of the world may hold one small blob of a single ore, chosen
    // from the band its height belongs to. A block belongs to the first vein that reaches it,
    // which keeps a lookup to the eight cells around it.
    class OreVeins {
    public:
        void init(std::uint64_t seed, int min_y, const Blocks& blocks) {
            seed_ = seed;
            min_y_ = min_y;
            blocks_ = blocks;
        }

        std::int32_t replace_deep(std::int32_t x, std::int32_t y, std::int32_t z,
            std::int32_t deep) const {
            if (deep != blocks_.stone) {
                return deep;
            }
            const std::int32_t cell_x = x >> 3;
            const std::int32_t cell_y = y >> 3;
            const std::int32_t cell_z = z >> 3;
            // A blob can only reach two blocks out of its own cell, so a neighbour only has
            // to be looked at when the block sits on that side of its cell. Interior blocks
            // cost one hash instead of twenty-seven.
            const int lx = x & 7;
            const int ly = y & 7;
            const int lz = z & 7;
            for (int dz = -1; dz <= 1; ++dz) {
                if ((dz < 0 && lz > 1) || (dz > 0 && lz < 6)) {
                    continue;
                }
                for (int dy = -1; dy <= 1; ++dy) {
                    if ((dy < 0 && ly > 1) || (dy > 0 && ly < 6)) {
                        continue;
                    }
                    for (int dx = -1; dx <= 1; ++dx) {
                        if ((dx < 0 && lx > 1) || (dx > 0 && lx < 6)) {
                            continue;
                        }
                        const std::uint64_t key = (static_cast<std::uint64_t>(
                            static_cast<std::uint32_t>(cell_x + dx)) << 42) ^
                            (static_cast<std::uint64_t>(
                                static_cast<std::uint32_t>(cell_y + dy)) << 21) ^
                            static_cast<std::uint64_t>(
                                static_cast<std::uint32_t>(cell_z + dz));
                        const std::uint64_t hash =
                            detail::hash64(key * 0x9E3779B97F4A7C15ull + seed_);
                        const std::int32_t ore = ore_for_band(static_cast<int>(hash % 7),
                            cell_y * 8 + 4);
                        if (ore == 0) {
                            continue;
                        }

                        const int cx = (cell_x + dx) * 8 + static_cast<int>((hash >> 8) % 8);
                        const int cy = (cell_y + dy) * 8 + static_cast<int>((hash >> 16) % 8);
                        const int cz = (cell_z + dz) * 8 + static_cast<int>((hash >> 24) % 8);
                        const int limit = 1 + static_cast<int>((hash >> 32) % 2);
                        if (std::abs(x - cx) <= limit && std::abs(y - cy) <= limit &&
                            std::abs(z - cz) <= limit) {
                            return ore;
                        }
                    }
                }
            }
            return blocks_.stone;
        }

    private:
        // Which ore a band holds; 0 means the vein is empty here. The bands are the ones the
        // game uses: most ores live in a height window rather than everywhere.
        std::int32_t ore_for_band(int slot, int y) const {
            switch (slot) {
            case 0: return (y > min_y_ + 6 && y < 110) ? blocks_.coal_ore : 0;
            case 1: return (y > min_y_ + 6 && y < 90) ? blocks_.copper_ore : 0;
            case 2: return (y > min_y_ + 6 && y < 72) ? blocks_.iron_ore : 0;
            case 3: return (y > min_y_ + 6 && y < 40) ? blocks_.gold_ore : 0;
            case 4: return (y < 0) ? blocks_.redstone_ore : 0;
            case 5: return (y < 24) ? blocks_.lapis_ore : 0;
            case 6: return (y < 16) ? blocks_.diamond_ore : 0;
            default: return 0;
            }
        }

        Blocks blocks_{};
        std::uint64_t seed_ = 0;
        int min_y_ = -64;
    };

} // namespace terila::world::terrain
