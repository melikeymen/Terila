#pragma once

#include <cstdint>
#include <vector>

namespace terila::world {

    struct ChunkSection {
        std::uint16_t block_count = 0;
        std::uint16_t fluid_count = 0;
        std::uint8_t  block_bits = 0;                      // bits_per_entry; 0 = single
        std::vector<std::int32_t> block_palette;           // empty if bits==0 OR direct
        std::vector<std::int32_t> block_states;            // 4096; index (indirect) or value (direct)
        std::uint8_t  biome_bits = 0;
        std::vector<std::int32_t> biome_palette;
        std::vector<std::int32_t> biome_states;            // 64
    };

    struct Heightmap {
        std::int32_t type_id = 0;                          // 1=WORLD_SURFACE, 4=MOTION_BLOCKING, 5=MOTION_BLOCKING_NO_LEAVES
        std::vector<std::int64_t> longs;
    };

    struct LightData {
        std::vector<std::int64_t> sky_y_mask;
        std::vector<std::int64_t> block_y_mask;
        std::vector<std::int64_t> empty_sky_y_mask;
        std::vector<std::int64_t> empty_block_y_mask;
        std::vector<std::vector<std::uint8_t>> sky_updates;   // each 2048 bytes
        std::vector<std::vector<std::uint8_t>> block_updates;
    };

    struct Chunk {
        std::int32_t x = 0;
        std::int32_t z = 0;
        std::int32_t min_y = -64;
        std::int32_t section_count = 24;
        std::vector<Heightmap>    heightmaps;
        std::vector<ChunkSection> sections;
        LightData                 light;
    };

}