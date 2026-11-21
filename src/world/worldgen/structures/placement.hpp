#pragma once

// Where structures go. A set places at most one structure per spacing x spacing cell of
// chunks, keeps it at least `separation / 2` chunks away from the cell edge, and picks the
// chunk from the world seed and the set's salt, so the same seed always builds the same
// sites. Once a site is known the template's cells are written into whichever chunk of the
// world they land in, because a structure crosses chunk borders.

#include "world/terrain_noise.hpp"
#include "world/worldgen/structures/template.hpp"

#include <algorithm>
#include <cstdint>

namespace terila::world::worldgen {

    // `minecraft:random_spread` from structure_sets.json.
    struct StructureSpread {
        std::uint64_t salt = 0;
        int spacing = 34;
        int separation = 8;
    };

    struct StructureSite {
        bool valid = false;
        std::int32_t chunk_x = 0;
        std::int32_t chunk_z = 0;
    };

    // The chunk a cell of the spread grid starts its structure in.
    inline StructureSite spread_site(std::uint64_t seed, const StructureSpread& spread,
        std::int32_t grid_x, std::int32_t grid_z) {
        StructureSite site;
        if (spread.spacing <= 0) {
            return site;
        }
        const int inset = std::max(0, spread.separation) / 2;
        const int span = std::max(1, spread.spacing - std::max(0, spread.separation));
        const std::uint64_t mixed = terrain::detail::hash64(
            seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(grid_x)) << 32) ^
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(grid_z)) ^
            spread.salt * 0x9E3779B97F4A7C15ull);
        site.valid = true;
        site.chunk_x = grid_x * spread.spacing + inset +
            static_cast<std::int32_t>(mixed % static_cast<std::uint64_t>(span));
        site.chunk_z = grid_z * spread.spacing + inset +
            static_cast<std::int32_t>((mixed >> 32) % static_cast<std::uint64_t>(span));
        return site;
    }

    // Which cell of the spread grid a chunk belongs to, for the caller that asks "does a
    // structure near this chunk cover it?".
    inline std::int32_t spread_cell(std::int32_t chunk, int spacing) {
        if (spacing <= 0) {
            return 0;
        }
        std::int32_t cell = chunk / spacing;
        if (chunk < 0 && chunk % spacing != 0) {
            cell -= 1;
        }
        return cell;
    }

    // Writes the part of a template that lands in one chunk. The palette's air is skipped:
    // a structure carves its rooms out of the terrain with its own blocks, not by deleting
    // what the generator put there.
    template<class Write>
    int apply_template(const StructureTemplate& tmpl, std::int32_t origin_x,
        std::int32_t origin_y, std::int32_t origin_z, std::int32_t chunk_x,
        std::int32_t chunk_z, Write&& write) {
        int placed = 0;
        for (const TemplateCell& cell : tmpl.cells) {
            if (cell.state == 0) {
                continue;
            }
            const std::int32_t wx = origin_x + cell.x;
            const std::int32_t wy = origin_y + cell.y;
            const std::int32_t wz = origin_z + cell.z;
            if ((wx >> 4) != chunk_x || (wz >> 4) != chunk_z) {
                continue;
            }
            write(wx, wy, wz, cell.state);
            ++placed;
        }
        return placed;
    }

    // How far below a structure the ground may be before the gap under it is left alone: a
    // structure on a cliff edge would otherwise stand on a column taller than a house.
    inline constexpr std::int32_t kMaxBeard = 24;

    // Fills the gap a template leaves over a slope. Its lowest layer is the ground the
    // structure stands on, so every column under it is raised to that layer with the given
    // filler and the structure never hangs in the air. Filling before the blocks are written
    // lets the structure keep its own buried part.
    template<class Surface, class Write>
    int fill_beard(const StructureTemplate& tmpl, std::int32_t origin_x,
        std::int32_t origin_y, std::int32_t origin_z, std::int32_t chunk_x,
        std::int32_t chunk_z, Surface&& surface, std::int32_t filler, Write&& write) {
        std::int32_t lowest = tmpl.size_y;
        for (const TemplateCell& cell : tmpl.cells) {
            if (cell.state != 0 && cell.y < lowest) {
                lowest = cell.y;
            }
        }
        if (lowest >= tmpl.size_y) {
            return 0;  // No solid layer to stand on.
        }
        const std::int32_t base_y = origin_y + lowest;
        int filled = 0;
        for (std::int32_t dz = 0; dz < tmpl.size_z; ++dz) {
            const std::int32_t wz = origin_z + dz;
            if ((wz >> 4) != chunk_z) continue;
            for (std::int32_t dx = 0; dx < tmpl.size_x; ++dx) {
                const std::int32_t wx = origin_x + dx;
                if ((wx >> 4) != chunk_x) continue;
                const std::int32_t ground = surface(wx, wz);
                if (base_y - 1 > ground + kMaxBeard) continue;
                for (std::int32_t wy = ground + 1; wy < base_y; ++wy) {
                    write(wx, wy, wz, filler);
                    ++filled;
                }
            }
        }
        return filled;
    }

} // namespace terila::world::worldgen
