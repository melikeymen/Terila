#pragma once

// The blocks and biomes the overworld generator paints with, and the lookup rules that turn
// registry names into the ids the chunk format stores.

#include "world/light_engine.hpp"
#include "log/logger.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace terila::world::terrain {

    using IdResolver = std::function<std::optional<std::int32_t>(std::string_view)>;

    struct Blocks {
        std::int32_t air = 0;
        std::int32_t bedrock = 0;
        std::int32_t stone = 0;
        std::int32_t dirt = 0;
        std::int32_t coarse_dirt = 0;
        std::int32_t grass = 0;
        std::int32_t sand = 0;
        std::int32_t red_sand = 0;
        std::int32_t sandstone = 0;
        std::int32_t terracotta = 0;
        std::int32_t gravel = 0;
        std::int32_t water = 0;
        std::int32_t ice = 0;
        std::int32_t snow = 0;
        std::int32_t oak_log = 0;
        std::int32_t oak_leaves = 0;
        std::int32_t birch_log = 0;
        std::int32_t birch_leaves = 0;
        std::int32_t spruce_log = 0;
        std::int32_t spruce_leaves = 0;
        std::int32_t jungle_log = 0;
        std::int32_t jungle_leaves = 0;
        std::int32_t acacia_log = 0;
        std::int32_t acacia_leaves = 0;
        std::int32_t cactus = 0;
        // Ores, deepest first; a missing id falls back to stone so the vein never appears.
        std::int32_t coal_ore = 0;
        std::int32_t copper_ore = 0;
        std::int32_t iron_ore = 0;
        std::int32_t gold_ore = 0;
        std::int32_t redstone_ore = 0;
        std::int32_t lapis_ore = 0;
        std::int32_t diamond_ore = 0;
    };

    // What a single block is, for the rules that do not care about its identity: the natural
    // spawner tests a column like this instead of resolving block ids itself.
    enum class BlockKind : std::uint8_t {
        Air,     // no collision, no light loss
        Soil,    // animals_spawnable_on (grass_block)
        Solid,   // a full collision block
        Water,   // the water fluid
        Plant,   // snow, leaves: a thin layer a mob stands on top of
    };

    // One entry of the biome table: what the surface looks like and what grows on it.
    struct Biome {
        std::int32_t id = 0;          // biome registry id
        std::int32_t surface = 0;     // top block
        std::int32_t filler = 0;      // three blocks under the surface
        std::int32_t deep = 0;        // everything below the filler
        std::int32_t log = 0;
        std::int32_t leaves = 0;
        int tree_chance = 0;          // one tree per N columns, 0 disables trees
        int cactus_chance = 0;        // one cactus per N columns
        bool snow_cap = false;        // a snow layer on top of the surface
        int snow_above = 0;           // snow above this surface height, 0 disables
        bool ice_top = false;         // a frozen surface at sea level
    };

    // The biome table order the climate indexes into: water first, then the shores, then the
    // dry land from cold to hot.
    enum BiomeIndex : std::size_t {
        kDeepOcean = 0, kOcean, kFrozenOcean, kBeach, kStonyShore, kSnowyPlains,
        kSnowyTaiga, kTaiga, kWindsweptHills, kStonyPeaks, kMeadow, kPlains, kForest,
        kBirchForest, kSwamp, kJungle, kSavanna, kBadlands, kDesert, kBiomeCount
    };

    // The same order as ids, so a biome index can be looked up in the game data
    // (spawn lists, biome attributes) by name.
    inline constexpr const char* kBiomeNames[kBiomeCount] = {
        "minecraft:deep_ocean",
        "minecraft:ocean",
        "minecraft:frozen_ocean",
        "minecraft:beach",
        "minecraft:stony_shore",
        "minecraft:snowy_plains",
        "minecraft:snowy_taiga",
        "minecraft:taiga",
        "minecraft:windswept_hills",
        "minecraft:stony_peaks",
        "minecraft:meadow",
        "minecraft:plains",
        "minecraft:forest",
        "minecraft:birch_forest",
        "minecraft:swamp",
        "minecraft:jungle",
        "minecraft:savanna",
        "minecraft:badlands",
        "minecraft:desert",
    };

    inline std::optional<Blocks> resolve_blocks(const IdResolver& block) {
        Blocks blocks;
        bool complete = true;
        const auto id = [&](const char* name) {
            const auto resolved = block(name);
            if (!resolved) {
                LOG_WARN(Chunk, "terrain: cannot resolve block '{}'", name);
                complete = false;
                return 0;
            }
            return *resolved;
        };
        // Blocks that only decorate some biomes may be missing: that biome falls back to a
        // block the world always has.
        const auto soft_id = [&](const char* name, std::int32_t fallback) {
            const auto resolved = block(name);
            if (!resolved) {
                LOG_DEBUG(Chunk, "terrain: block '{}' is missing, using {}", name, fallback);
                return fallback;
            }
            return *resolved;
        };

        blocks.air = id("minecraft:air");
        blocks.bedrock = id("minecraft:bedrock");
        blocks.stone = id("minecraft:stone");
        blocks.dirt = id("minecraft:dirt");
        blocks.grass = id("minecraft:grass_block");
        blocks.sand = id("minecraft:sand");
        blocks.sandstone = id("minecraft:sandstone");
        blocks.gravel = id("minecraft:gravel");
        blocks.water = id("minecraft:water");
        blocks.snow = id("minecraft:snow");
        blocks.oak_log = id("minecraft:oak_log");
        blocks.oak_leaves = id("minecraft:oak_leaves");
        blocks.spruce_log = id("minecraft:spruce_log");
        blocks.spruce_leaves = id("minecraft:spruce_leaves");
        blocks.acacia_log = id("minecraft:acacia_log");
        blocks.acacia_leaves = id("minecraft:acacia_leaves");
        blocks.cactus = id("minecraft:cactus");

        blocks.coarse_dirt = soft_id("minecraft:coarse_dirt", blocks.dirt);
        blocks.red_sand = soft_id("minecraft:red_sand", blocks.sand);
        blocks.terracotta = soft_id("minecraft:terracotta", blocks.sand);
        blocks.ice = soft_id("minecraft:ice", blocks.water);
        blocks.birch_log = soft_id("minecraft:birch_log", blocks.oak_log);
        blocks.birch_leaves = soft_id("minecraft:birch_leaves", blocks.oak_leaves);
        blocks.jungle_log = soft_id("minecraft:jungle_log", blocks.oak_log);
        blocks.jungle_leaves = soft_id("minecraft:jungle_leaves", blocks.oak_leaves);
        blocks.coal_ore = soft_id("minecraft:coal_ore", blocks.stone);
        blocks.copper_ore = soft_id("minecraft:copper_ore", blocks.stone);
        blocks.iron_ore = soft_id("minecraft:iron_ore", blocks.stone);
        blocks.gold_ore = soft_id("minecraft:gold_ore", blocks.stone);
        blocks.redstone_ore = soft_id("minecraft:redstone_ore", blocks.stone);
        blocks.lapis_ore = soft_id("minecraft:lapis_ore", blocks.stone);
        blocks.diamond_ore = soft_id("minecraft:diamond_ore", blocks.stone);

        if (!complete) {
            return std::nullopt;
        }
        return blocks;
    }

    // Fills the table in BiomeIndex order. False when a biome is not in the registry, which
    // means the world cannot be streamed with the biome ids the client knows.
    inline bool resolve_biomes(const IdResolver& biome, const Blocks& blocks,
        std::vector<Biome>& out) {
        out.clear();
        out.reserve(kBiomeCount);
        bool complete = true;
        const auto add = [&](const char* name, std::int32_t surface, std::int32_t filler,
            std::int32_t deep, std::int32_t log, std::int32_t leaves, int trees, int cacti,
            bool snow, int snow_above = 0, bool ice = false) {
            const auto resolved = biome(name);
            if (!resolved) {
                LOG_WARN(Chunk, "terrain: cannot resolve biome '{}'", name);
                out.push_back(Biome{});
                complete = false;
                return;
            }
            out.push_back(Biome{ *resolved, surface, filler, deep, log, leaves, trees,
                cacti, snow, snow_above, ice });
        };

        add("minecraft:deep_ocean", blocks.gravel, blocks.gravel, blocks.stone,
            0, 0, 0, 0, false);
        add("minecraft:ocean", blocks.gravel, blocks.gravel, blocks.stone,
            0, 0, 0, 0, false);
        add("minecraft:frozen_ocean", blocks.gravel, blocks.gravel, blocks.stone,
            0, 0, 0, 0, true, 0, true);
        add("minecraft:beach", blocks.sand, blocks.sand, blocks.sandstone,
            0, 0, 0, 0, false);
        add("minecraft:stony_shore", blocks.stone, blocks.stone, blocks.stone,
            0, 0, 0, 0, false);
        add("minecraft:snowy_plains", blocks.grass, blocks.dirt, blocks.stone,
            blocks.spruce_log, blocks.spruce_leaves, 150, 0, true);
        add("minecraft:snowy_taiga", blocks.grass, blocks.dirt, blocks.stone,
            blocks.spruce_log, blocks.spruce_leaves, 40, 0, true);
        add("minecraft:taiga", blocks.grass, blocks.dirt, blocks.stone,
            blocks.spruce_log, blocks.spruce_leaves, 26, 0, false);
        add("minecraft:windswept_hills", blocks.grass, blocks.dirt, blocks.stone,
            blocks.spruce_log, blocks.spruce_leaves, 110, 0, false, 150);
        add("minecraft:stony_peaks", blocks.stone, blocks.stone, blocks.stone,
            0, 0, 0, 0, false, 120);
        add("minecraft:meadow", blocks.grass, blocks.dirt, blocks.stone,
            blocks.oak_log, blocks.oak_leaves, 200, 0, false);
        add("minecraft:plains", blocks.grass, blocks.dirt, blocks.stone,
            blocks.oak_log, blocks.oak_leaves, 120, 0, false);
        add("minecraft:forest", blocks.grass, blocks.dirt, blocks.stone,
            blocks.oak_log, blocks.oak_leaves, 18, 0, false);
        add("minecraft:birch_forest", blocks.grass, blocks.dirt, blocks.stone,
            blocks.birch_log, blocks.birch_leaves, 20, 0, false);
        add("minecraft:swamp", blocks.grass, blocks.coarse_dirt, blocks.stone,
            blocks.oak_log, blocks.oak_leaves, 70, 0, false);
        add("minecraft:jungle", blocks.grass, blocks.dirt, blocks.stone,
            blocks.jungle_log, blocks.jungle_leaves, 14, 0, false);
        add("minecraft:savanna", blocks.grass, blocks.dirt, blocks.stone,
            blocks.acacia_log, blocks.acacia_leaves, 90, 0, false);
        add("minecraft:badlands", blocks.red_sand, blocks.terracotta, blocks.stone,
            0, 0, 0, 60, false);
        add("minecraft:desert", blocks.sand, blocks.sand, blocks.sandstone,
            0, 0, 0, 40, false);

        return complete && out.size() == kBiomeCount;
    }

    // What the blocks this generator places do to light. Everything else stays opaque: no
    // light passes and none is emitted.
    inline LightRules light_rules_for(const Blocks& blocks) {
        LightRules rules;
        rules.set(blocks.air, LightBlock{ 0, 0, true });
        rules.set(blocks.water, LightBlock{ 0, 1, false });
        rules.set(blocks.snow, LightBlock{ 0, 0, true });   // the thin layer
        rules.set(blocks.ice, LightBlock{ 0, 2, false });
        for (const auto leaves : { blocks.oak_leaves, blocks.spruce_leaves,
            blocks.acacia_leaves, blocks.birch_leaves, blocks.jungle_leaves }) {
            rules.set(leaves, LightBlock{ 0, 1, false });
        }
        return rules;
    }

} // namespace terila::world::terrain
