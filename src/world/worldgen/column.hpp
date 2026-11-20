#pragma once

// One column of the heightmap and the cache that keeps it around.
//
// A column is what the surface, the light pass and the material rules ask about: where the
// land is, which biome paints it and the values a rule may test. Building one walks the
// density field, so it is the unit both the shared cell cache and the per-thread column
// table store.

#include "world/terrain_noise.hpp"

#include <array>
#include <cstdint>

namespace terila::world::worldgen {

    struct Column {
        int height = 0;
        int biome = 0;
        float temperature = 0.0f;
        int surface_depth = 3;
        double surface_secondary = 0.0;
        int preliminary = 0;
        // Memoised sky top: the light pass asks every cell of the halo for it.
        int sky = kNoSky;
        static constexpr int kNoSky = -1000000;
    };

    // Columns are looked up hundreds of times per chunk (the light pass asks for the sky
    // above every cell of the halo), so the cache is a small direct-mapped table instead of a
    // hash map: one probe, no allocation.
    class ColumnCache {
    public:
        ColumnCache() {
            keys_.fill(kEmpty);
            cell_keys_.fill(kEmpty);
        }

        const Column* find(std::int32_t x, std::int32_t z) const {
            const std::size_t slot = slot_of(key(x, z));
            return keys_[slot] == key(x, z) ? &columns_[slot] : nullptr;
        }

        void insert(std::int32_t x, std::int32_t z, const Column& column) {
            const std::size_t slot = slot_of(key(x, z));
            keys_[slot] = key(x, z);
            columns_[slot] = column;
        }

        const Column* find_cell(std::int32_t cell_x, std::int32_t cell_z) const {
            const std::size_t slot = slot_of(key(cell_x, cell_z));
            return cell_keys_[slot] == key(cell_x, cell_z) ? &cells_[slot] : nullptr;
        }

        void set_sky(std::int32_t x, std::int32_t z, int value) {
            const std::size_t slot = slot_of(key(x, z));
            if (keys_[slot] == key(x, z)) {
                columns_[slot].sky = value;
            }
        }

        void insert_cell(std::int32_t cell_x, std::int32_t cell_z, const Column& column) {
            const std::size_t slot = slot_of(key(cell_x, cell_z));
            cell_keys_[slot] = key(cell_x, cell_z);
            cells_[slot] = column;
        }

    private:
        static constexpr std::uint64_t kEmpty = ~0ull;
        // The light pass asks for the sky of a fifteen block halo, so the table holds a few
        // thousand columns without evicting the coarser cells they are built from.
        static constexpr std::size_t kSlots = 8192;

        static std::uint64_t key(std::int32_t x, std::int32_t z) {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                static_cast<std::uint32_t>(z);
        }

        static std::size_t slot_of(std::uint64_t key) {
            return static_cast<std::size_t>(terrain::detail::hash64(key)) & (kSlots - 1);
        }

        std::array<std::uint64_t, kSlots> keys_{};
        std::array<Column, kSlots> columns_{};
        std::array<std::uint64_t, kSlots> cell_keys_{};
        std::array<Column, kSlots> cells_{};
    };

} // namespace terila::world::worldgen
