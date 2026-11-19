// Dumps one generated chunk as plain text so the parity harness can diff it against a
// region file the real server wrote.
//
//   worldgen_dump <input-dir> <seed> <chunk-x> <chunk-z> <out-file>
//
// Blocks are written as "<world-x> <y> <world-z> <state-id>", biomes as
// "b <world-x> <y> <world-z> <biome-id>" at the 4x4x4 cell centres the chunk format stores.

#include "data/input_store.hpp"
#include "data/registry_loader.hpp"
#include "world/chunk.hpp"
#include "world/block_world.hpp"
#include "world/terrain_generator.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s <input-dir> <seed> <cx> <cz> <out-file>\n", argv[0]);
        return 2;
    }

    const std::string input = argv[1];
    const auto seed = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const std::int32_t cx = std::atoi(argv[3]);
    const std::int32_t cz = std::atoi(argv[4]);

    terila::data::input::mount(input);

    terila::data::RegistryData data;
    terila::data::load_blocks(data, "blocks/state_ids.json");
    terila::data::load_biome_ids(data.biome_ids, "worldgen/biomes.json");
    terila::data::DimensionInfo dimension;
    terila::data::load_dimensions(dimension, "worldgen/dimensions.json");

    const auto block_id = [&data](std::string_view name) -> std::optional<std::int32_t> {
        const auto hit = data.block_state_ids.find(std::string(name));
        return hit == data.block_state_ids.end() ? std::nullopt
            : std::optional<std::int32_t>(hit->second);
    };
    const auto biome_id = [&data](std::string_view name) -> std::optional<std::int32_t> {
        const auto hit = data.biome_ids.find(std::string(name));
        return hit == data.biome_ids.end() ? std::nullopt
            : std::optional<std::int32_t>(hit->second);
    };

    terila::world::terrain::Generator generator;
    if (!generator.init(dimension.min_y, dimension.height / 16, seed, block_id, biome_id)) {
        std::fprintf(stderr, "generator.init failed\n");
        return 3;
    }

    const terila::world::Chunk chunk = generator.generate(cx, cz);
    std::ofstream out(argv[5], std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", argv[5]);
        return 4;
    }

    const std::int32_t wx0 = cx * 16;
    const std::int32_t wz0 = cz * 16;
    for (int index = 0; index < chunk.section_count * 16; ++index) {
        const int y = chunk.min_y + index;
        const std::size_t section_index = static_cast<std::size_t>(index / 16);
        const auto& section = chunk.sections[section_index];
        if (section.block_palette.empty()) {
            continue;
        }
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                const std::int32_t state = terila::world::section_state(section,
                    (index % 16) * 256 + z * 16 + x);
                out << (wx0 + x) << ' ' << y << ' ' << (wz0 + z) << ' ' << state << '\n';
            }
        }
        for (int cy = 0; cy < 4; ++cy) {
            for (int czz = 0; czz < 4; ++czz) {
                for (int cxx = 0; cxx < 4; ++cxx) {
                    const int cell = (cy * 4 + czz) * 4 + cxx;
                    std::int32_t biome = 0;
                    if (section.biome_bits == 0) {
                        biome = section.biome_palette.empty()
                            ? 0 : section.biome_palette.front();
                    }
                    else {
                        const int bits = section.biome_bits;
                        const int bit = cell * bits;
                        const std::uint64_t word = static_cast<std::uint64_t>(
                            section.biome_states[static_cast<std::size_t>(bit / 64)]);
                        std::uint64_t value = word >> (bit % 64);
                        if (bit % 64 + bits > 64) {
                            value |= static_cast<std::uint64_t>(section.biome_states[
                                static_cast<std::size_t>(bit / 64) + 1]) << (64 - bit % 64);
                        }
                        const auto entry = static_cast<std::size_t>(
                            value & ((1ull << bits) - 1ull));
                        biome = entry < section.biome_palette.size()
                            ? section.biome_palette[entry] : 0;
                    }
                    out << "b " << (wx0 + cxx * 4 + 2) << ' '
                        << (chunk.min_y + section_index * 16 + cy * 4 + 2) << ' '
                        << (wz0 + czz * 4 + 2) << ' ' << biome << '\n';
                }
            }
        }
    }

    std::fprintf(stderr, "wrote chunk %d,%d for seed %u\n", cx, cz, seed);
    return 0;
}
