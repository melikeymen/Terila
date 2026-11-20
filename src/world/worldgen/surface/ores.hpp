#pragma once

// Ore and stone blobs: the veins that colour the filler (granite, diorite, andesite, tuff,
// gravel, clay, dirt) and the ores inside them. The game gives a chunk a fixed number of
// blobs per feature and walks each blob out as a short chain of blocks; a block belongs to
// the first blob that covers it. The blobs of one chunk are built once and shared by every
// column that asks about it, which keeps the cost per chunk far below a hash per block.

#include "world/terrain_biomes.hpp"
#include "world/terrain_noise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace terila::world::worldgen {

    class OreBlobs {
    public:
        void init(std::uint64_t seed, int min_y, int max_y, const terrain::Blocks& blocks) {
            seed_ = seed;
            min_y_ = min_y;
            max_y_ = max_y;
            blocks_ = blocks;
            placements_.clear();
            add(blocks.granite, 64, 2, 1, 0, 60);
            add(blocks.granite, 64, 1, 6, 64, 128);
            add(blocks.diorite, 64, 2, 1, 0, 60);
            add(blocks.diorite, 64, 1, 6, 64, 128);
            add(blocks.andesite, 64, 2, 1, 0, 60);
            add(blocks.andesite, 64, 1, 6, 64, 128);
            add(blocks.tuff, 64, 2, 1, min_y_, 0);
            add(blocks.gravel, 33, 14, 1, min_y_, max_y_);
            add(blocks.clay, 33, 1, 1, min_y_, max_y_);
            add(blocks.dirt, 33, 7, 1, 0, 160);
        }

        // The state a stone block becomes, or 0 when no blob covers it.
        std::int32_t blob_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            if (placements_.empty()) {
                return 0;
            }
            const std::int32_t cx = x >> 4;
            const std::int32_t cz = z >> 4;
            const Layer& layer = layer_for(cx, cz);
            if (layer.blocks.empty()) {
                return 0;
            }
            const std::uint64_t key = pack(x & 15, y, z & 15);
            const auto found = layer.blocks.find(key);
            return found == layer.blocks.end() ? 0 : found->second;
        }

        // Deep filler of a column: a blob first, then the ore veins of the band.
        std::int32_t replace_deep(std::int32_t x, std::int32_t y, std::int32_t z,
            std::int32_t deep) const {
            if (deep != blocks_.stone && deep != blocks_.deepslate) {
                return deep;
            }
            if (const std::int32_t blob = blob_at(x, y, z); blob != 0) {
                return blob;
            }
            if (deep != blocks_.stone || blocks_.coal_ore == blocks_.stone) {
                return deep;
            }

            const std::int32_t cell_x = x >> 3;
            const std::int32_t cell_y = y >> 3;
            const std::int32_t cell_z = z >> 3;
            // A vein reaches at most two blocks out of its cell, so a neighbour only has to be
            // looked at when the block sits on that side of its cell.
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
                            terrain::detail::hash64(key * 0x9E3779B97F4A7C15ull + seed_);
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
        // One placed ore feature: `count` blobs per chunk, one in `rarity`, of `size` blocks.
        struct Placement {
            std::int32_t state = 0;
            int size = 0;
            int count = 0;
            int rarity = 1;
            int min_y = 0;
            int max_y = 0;
        };

        struct Layer {
            const void* owner = nullptr;
            std::int32_t cx = 0;
            std::int32_t cz = 0;
            std::uint64_t stamp = 0;
            std::unordered_map<std::uint64_t, std::int32_t> blocks;
        };

        void add(std::int32_t state, int size, int count, int rarity, int min_y, int max_y) {
            if (state == 0) {
                return;
            }
            placements_.push_back({ state, size, count, rarity, min_y, max_y });
        }

        static std::uint64_t pack(int lx, std::int32_t y, int lz) {
            return (static_cast<std::uint64_t>(lx) << 24) |
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y + 1024) & 0x3FF) << 12) |
                static_cast<std::uint64_t>(lz);
        }

        static std::array<Layer, 8>& layers() {
            static thread_local std::array<Layer, 8> table{};
            return table;
        }

        const Layer& layer_for(std::int32_t cx, std::int32_t cz) const {
            static thread_local std::uint64_t clock = 0;
            std::array<Layer, 8>& table = layers();
            Layer* victim = &table[0];
            for (Layer& layer : table) {
                if (layer.owner == this && layer.cx == cx && layer.cz == cz) {
                    layer.stamp = ++clock;
                    return layer;
                }
                if (layer.owner != this || layer.stamp < victim->stamp) {
                    victim = &layer;
                }
            }
            victim->owner = this;
            victim->cx = cx;
            victim->cz = cz;
            victim->stamp = ++clock;
            victim->blocks.clear();
            build(*victim);
            return *victim;
        }

        void build(Layer& layer) const {
            const std::uint32_t cx = static_cast<std::uint32_t>(layer.cx);
            const std::uint32_t cz = static_cast<std::uint32_t>(layer.cz);
            for (std::size_t index = 0; index < placements_.size(); ++index) {
                const Placement& placement = placements_[index];
                for (int blob = 0; blob < placement.count; ++blob) {
                    const std::uint64_t salt = static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ull ^
                        (static_cast<std::uint64_t>(cx) << 32) ^ static_cast<std::uint64_t>(cz) ^
                        (static_cast<std::uint64_t>(blob) << 8);
                    std::uint64_t hash = terrain::detail::hash64(salt * 0x2545F4914F6CDD1Dull + seed_);
                    if (placement.rarity > 1 && hash % static_cast<std::uint64_t>(placement.rarity) != 0) {
                        continue;
                    }

                    const int span = std::max(1, placement.max_y - placement.min_y + 1);
                    double x = static_cast<double>(layer.cx * 16 + static_cast<int>((hash >> 4) % 16)) + 0.5;
                    double z = static_cast<double>(layer.cz * 16 + static_cast<int>((hash >> 9) % 16)) + 0.5;
                    double y = static_cast<double>(placement.min_y + static_cast<int>((hash >> 14) % static_cast<std::uint64_t>(span))) + 0.5;

                    // The blob is a short random walk: the game draws a direction and a size,
                    // then thickens the walked line by one block in every direction.
                    const int steps = std::max(4, placement.size / 6);
                    for (int step = 0; step < steps; ++step) {
                        hash = terrain::detail::hash64(hash + 0x9E3779B97F4A7C15ull);
                        const int px = static_cast<int>(std::floor(x));
                        const int py = static_cast<int>(std::floor(y));
                        const int pz = static_cast<int>(std::floor(z));
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dz = -1; dz <= 1; ++dz) {
                                for (int dx = -1; dx <= 1; ++dx) {
                                    if (dx * dx + dy * dy + dz * dz > 2) {
                                        continue;
                                    }
                                    const int bx = px + dx;
                                    const int by = py + dy;
                                    const int bz = pz + dz;
                                    if (by < min_y_ || by > max_y_) {
                                        continue;
                                    }
                                    // Vanilla clips a feature to its chunk.
                                    const std::int32_t bx_chunk = bx >> 4;
                                    const std::int32_t bz_chunk = bz >> 4;
                                    if (bx_chunk != layer.cx || bz_chunk != layer.cz) {
                                        continue;
                                    }
                                    layer.blocks.emplace(
                                        pack(bx & 15, by, bz & 15), placement.state);
                                }
                            }
                        }
                        x += (static_cast<double>((hash >> 8) & 0xFF) / 255.0 - 0.5) * 2.0;
                        y += (static_cast<double>((hash >> 16) & 0xFF) / 255.0 - 0.5) * 1.2;
                        z += (static_cast<double>((hash >> 24) & 0xFF) / 255.0 - 0.5) * 2.0;
                    }
                }
            }
        }

        // Which ore a band holds; 0 means the vein is empty here. The bands are the height
        // windows the game uses rather than one ore everywhere.
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

        terrain::Blocks blocks_{};
        std::uint64_t seed_ = 0;
        int min_y_ = -64;
        int max_y_ = 320;
        std::vector<Placement> placements_;
    };

} // namespace terila::world::worldgen
