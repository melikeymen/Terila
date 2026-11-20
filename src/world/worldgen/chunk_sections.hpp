#pragma once

// Packing a finished column into the sections the client reads: one per sixteen blocks of
// height, each carrying its own block and biome palettes. The fill stage hands over a flat
// block array, so this file only decides how to store what is already there.

#include "world/chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace terila::world::worldgen {

    // `biome_at` answers the biome id of one block position. The client stores biomes at a
    // quarter of the block resolution, so a section samples a four by four grid of them.
    template<class BiomeAt>
    void write_sections(Chunk& chunk, const std::vector<std::int32_t>& blocks, std::int32_t wx0,
        std::int32_t wz0, int min_y, int section_count, std::int32_t air, std::int32_t water,
        BiomeAt&& biome_at) {
        chunk.sections.resize(static_cast<std::size_t>(section_count));
        for (int sy = 0; sy < section_count; ++sy) {
            ChunkSection& section = chunk.sections[static_cast<std::size_t>(sy)];
            std::vector<std::int32_t> palette;
            std::vector<std::int32_t> indices(4096, 0);
            std::int32_t last_state = -1;
            std::int32_t last_index = 0;
            int non_air = 0;
            int fluids = 0;

            // One pass over the section: the palette is grown as states appear, and the index
            // of the previous block is reused while the state does not change, which is what
            // makes open air and solid ground cheap.
            for (int i = 0; i < 4096; ++i) {
                const std::int32_t state = blocks[static_cast<std::size_t>(sy) * 4096 +
                    static_cast<std::size_t>(i)];
                if (state != last_state) {
                    std::int32_t found = -1;
                    for (std::size_t k = 0; k < palette.size(); ++k) {
                        if (palette[k] == state) {
                            found = static_cast<std::int32_t>(k);
                            break;
                        }
                    }
                    if (found < 0) {
                        found = static_cast<std::int32_t>(palette.size());
                        palette.push_back(state);
                    }
                    last_state = state;
                    last_index = found;
                }
                indices[static_cast<std::size_t>(i)] = last_index;
                if (state != air) ++non_air;
                if (state == water) ++fluids;
            }

            section.block_count = static_cast<std::uint16_t>(non_air);
            section.fluid_count = static_cast<std::uint16_t>(fluids);
            if (palette.size() == 1) {
                section.block_bits = 0;
                section.block_palette = { palette[0] };
            }
            else {
                int bits = 4;
                while (bits < 8 && (1u << bits) < palette.size()) {
                    ++bits;
                }
                section.block_bits = static_cast<std::uint8_t>(bits);
                section.block_palette = palette;
                section.block_states = indices;
            }

            std::vector<std::int32_t> biome_palette;
            std::vector<std::int32_t> biome_indices(64, 0);
            for (int cy = 0; cy < 4; ++cy) {
                for (int cz = 0; cz < 4; ++cz) {
                    for (int cxx = 0; cxx < 4; ++cxx) {
                        const std::int32_t biome_id = biome_at(wx0 + cxx * 4 + 2,
                            min_y + sy * 16 + cy * 4 + 2, wz0 + cz * 4 + 2);
                        std::int32_t index = -1;
                        for (std::size_t k = 0; k < biome_palette.size(); ++k) {
                            if (biome_palette[k] == biome_id) {
                                index = static_cast<std::int32_t>(k);
                                break;
                            }
                        }
                        if (index < 0) {
                            index = static_cast<std::int32_t>(biome_palette.size());
                            biome_palette.push_back(biome_id);
                        }
                        biome_indices[static_cast<std::size_t>((cy * 4 + cz) * 4 + cxx)] = index;
                    }
                }
            }
            if (biome_palette.size() == 1) {
                section.biome_bits = 0;
                section.biome_palette = { biome_palette[0] };
            }
            else {
                int bits = 1;
                while (bits < 4 && (1u << bits) < biome_palette.size()) {
                    ++bits;
                }
                section.biome_bits = static_cast<std::uint8_t>(bits);
                section.biome_palette = biome_palette;
                section.biome_states = biome_indices;
            }
        }
    }

} // namespace terila::world::worldgen
