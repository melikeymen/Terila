#pragma once

#include "world/chunk.hpp"
#include "log/logger.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terila::world::flat {

    using IdResolver = std::function<std::optional<std::int32_t>(std::string_view)>;

    struct FlatSpec {
        std::string air = "minecraft:air";
        std::string bedrock = "minecraft:bedrock";
        std::string dirt = "minecraft:dirt";
        std::string grass = "minecraft:grass_block";
        std::string biome = "minecraft:plains";
        int         dirt_layers = 3;
        int         min_y = -64;
        int         section_count = 24;
    };

    inline std::optional<Chunk> generate(
        std::int32_t cx, std::int32_t cz,
        const FlatSpec& spec,
        const IdResolver& resolve_block,
        const IdResolver& resolve_biome)
    {
        const auto bedrock_opt = resolve_block(spec.bedrock);
        const auto dirt_opt = resolve_block(spec.dirt);
        const auto grass_opt = resolve_block(spec.grass);
        const auto biome_opt = resolve_biome(spec.biome);

        if (!bedrock_opt || !dirt_opt || !grass_opt) {
            LOG_WARN(Chunk, "flat_generator: cannot resolve blocks");
            return std::nullopt;
        }
        if (!biome_opt) {
            LOG_WARN(Chunk, "flat_generator: cannot resolve biome '{}'", spec.biome);
            return std::nullopt;
        }
        if (spec.dirt_layers < 0 || spec.dirt_layers > 13) {
            LOG_WARN(Chunk, "flat_generator: dirt_layers out of range");
            return std::nullopt;
        }

        const auto air_opt = resolve_block(spec.air);
        const std::int32_t air_id = air_opt.value_or(0);
        const std::int32_t bedrock_id = *bedrock_opt;
        const std::int32_t dirt_id = *dirt_opt;
        const std::int32_t grass_id = *grass_opt;
        const std::int32_t biome_id = *biome_opt;

        if (spec.section_count < 2 || spec.section_count > 32) {
            LOG_WARN(Chunk, "flat_generator: section_count out of range ({})", spec.section_count);
            return std::nullopt;
        }
        if (spec.section_count != 24) {
            LOG_WARN(Chunk, "flat_generator: heightmaps assume 384 blocks, got {} sections",
                spec.section_count);
        }

        Chunk c;
        c.x = cx;
        c.z = cz;
        c.min_y = spec.min_y;
        c.section_count = spec.section_count;

        const int grass_local_y = spec.dirt_layers + 1;

        // Heightmaps hold nine bits per column: the y of the first air block above
        // the surface, measured from min_y. 256 entries x 9 bits = 36 longs.
        const std::uint64_t column_height =
            static_cast<std::uint64_t>(grass_local_y + 1) & 0x1FF;
        std::vector<std::int64_t> height_longs(36, 0);
        for (int column = 0; column < 256; ++column) {
            const int bit = column * 9;
            const int long_index = bit / 64;
            const int offset = bit % 64;
            height_longs[static_cast<std::size_t>(long_index)] |=
                static_cast<std::int64_t>(column_height << offset);
            if (offset > 55) {
                height_longs[static_cast<std::size_t>(long_index) + 1] |=
                    static_cast<std::int64_t>(column_height >> (64 - offset));
            }
        }

        for (int type : {1, 4, 5}) {
            Heightmap hm;
            hm.type_id = type;
            hm.longs = height_longs;
            c.heightmaps.push_back(std::move(hm));
        }

        // Uniform air + plains everywhere
        c.sections.resize(static_cast<std::size_t>(c.section_count));
        for (auto& sec : c.sections) {
            sec.block_bits = 0;
            sec.block_palette = { air_id };
            sec.block_states.assign(4096, 0);
            sec.biome_bits = 0;
            sec.biome_palette = { biome_id };
            sec.biome_states.assign(64, 0);
            sec.block_count = 0;
        }

        // Bottom section: bedrock / dirt / grass / air
        ChunkSection& bottom = c.sections[0];
        bottom.block_bits = 4;
        bottom.block_palette = { air_id, bedrock_id, dirt_id, grass_id };
        bottom.block_states.assign(4096, 0);

        int non_air = 0;
        for (int y = 0; y <= grass_local_y && y < 16; ++y) {
            std::int32_t idx = 0;
            if (y == 0)                idx = 1;
            else if (y < grass_local_y) idx = 2;
            else                        idx = 3;

            non_air += 256;
            for (int zz = 0; zz < 16; ++zz)
                for (int xx = 0; xx < 16; ++xx)
                    bottom.block_states[static_cast<std::size_t>((y * 16 + zz) * 16 + xx)] = idx;
        }
        bottom.block_count = static_cast<std::uint16_t>(non_air);

        // Light mask bit i addresses section i-1: bit 0 is the section below the world.
        const int n = spec.section_count;
        c.light.sky_y_mask = { 6LL };                              // bits 1,2 = sections 0,1

        // Section 0 holds the terrain, so only the blocks at or above the surface
        // see the sky; filling it with 0xFF would light up the underground.
        std::vector<std::uint8_t> bottom_sky(2048, 0);
        for (int y = grass_local_y; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    const int index = (y * 16 + z) * 16 + x;
                    const auto byte_index = static_cast<std::size_t>(index / 2);
                    if (index % 2 == 0) bottom_sky[byte_index] |= 0x0F;
                    else                bottom_sky[byte_index] |= 0xF0;
                }
            }
        }
        c.light.sky_updates = { bottom_sky, std::vector<std::uint8_t>(2048, 0xFF) };
        c.light.empty_sky_y_mask = { (1LL << (n + 1)) - 8LL };     // bits 3..n = sections 2..n-1

        // No block light anywhere
        c.light.block_y_mask.clear();
        c.light.block_updates.clear();
        c.light.empty_block_y_mask = { (1LL << (n + 1)) - 2LL };   // bits 1..n

        return c;
    }

}