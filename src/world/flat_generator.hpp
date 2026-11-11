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

        Chunk c;
        c.x = cx;
        c.z = cz;
        c.min_y = -64;
        c.section_count = 24;

        for (int type : {1, 4, 5}) {
            Heightmap hm;
            hm.type_id = type;
            hm.longs.assign(37, 0);
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

        const int grass_local_y = spec.dirt_layers + 1;
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
        c.light.sky_y_mask = { 6LL };                    // bits 1,2 = sections 0,1
        c.light.sky_updates.assign(2, std::vector<std::uint8_t>(2048, 0xFF));
        c.light.empty_sky_y_mask = { 0x1FFFFF8LL };      // bits 3..24 = sections 2..23

        // No block light anywhere
        c.light.block_y_mask.clear();
        c.light.block_updates.clear();
        c.light.empty_block_y_mask = { 0x1FFFFFELL };    // bits 1..24, all sections empty

        return c;
    }

}