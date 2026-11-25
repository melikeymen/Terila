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
        std::int32_t deepslate = 0;
        std::int32_t tuff = 0;
        std::int32_t granite = 0;
        std::int32_t diorite = 0;
        std::int32_t andesite = 0;
        std::int32_t clay = 0;
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
        std::int32_t lava = 0;
        // The other dimensions' ground; the overworld falls back to stone.
        std::int32_t netherrack = 0;
        std::int32_t end_stone = 0;
        std::int32_t snow = 0;
        std::int32_t snow_block = 0;
        std::int32_t powder_snow = 0;
        std::int32_t packed_ice = 0;
        std::int32_t calcite = 0;
        std::int32_t podzol = 0;
        std::int32_t mycelium = 0;
        std::int32_t mud = 0;
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
        // Ground cover: one block tall plants that decorate a surface. Zero means the block
        // is not in this checkout's registry, and the biome then grows nothing there.
        std::int32_t short_grass = 0;
        std::int32_t fern = 0;
        std::int32_t dandelion = 0;
        std::int32_t poppy = 0;
        std::int32_t dead_bush = 0;
        // What grows under the sea, on the floor of an ocean column. Zero means the block is
        // not in this checkout's registry and the ocean stays bare.
        std::int32_t seagrass = 0;
        std::int32_t kelp = 0;
        std::int32_t kelp_plant = 0;
        // The reed that grows on a bank next to open water.
        std::int32_t sugar_cane = 0;
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

    // One tree a biome grows: the blocks it is made of and how tall its trunk stands. The
    // weight decides how often the species is picked, so a forest can mix birch into oak.
    struct TreeSpecies {
        std::int32_t log = 0;
        std::int32_t leaves = 0;
        int min_trunk = 4;
        int max_trunk = 6;
        int weight = 1;
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
        // The ground plant that stands on the surface block when the column's hash picks it:
        // one plant per cover_chance columns, and one flower per flower_chance columns that
        // takes the grass's place. Decided from the column's own coordinates so the chunk and
        // a single block read cannot disagree about it.
        int cover_chance = 0;
        std::int32_t cover = 0;
        int flower_chance = 0;
        std::int32_t flower = 0;
        // The provider the patch names, as an index into the generator's provider table. A
        // negative index means the biome's field was read from data without one, and the
        // single block above is what grows.
        int cover_provider = -1;
        int flower_provider = -1;
        // The species the column hash picks from, read from the biome's own placed features.
        // An empty list keeps the single log and leaf pair above.
        std::vector<TreeSpecies> species;
    };

    // The biome table order the climate indexes into: water first, then the shores, then the
    // dry land from cold to hot.
    enum BiomeIndex : std::size_t {
        kDeepOcean = 0, kOcean, kFrozenOcean, kBeach, kStonyShore, kSnowyPlains,
        kSnowyTaiga, kTaiga, kWindsweptHills, kStonyPeaks, kMeadow, kPlains, kForest,
        kBirchForest, kSwamp, kJungle, kSavanna, kBadlands, kDesert,
        // The other dimensions follow, so the overworld indices keep their meaning.
        kNetherWastes, kSoulSandValley, kCrimsonForest, kWarpedForest, kBasaltDeltas,
        kTheEnd, kEndHighlands, kEndMidlands, kEndBarrens, kSmallEndIslands,
        kBiomeCount
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
        "minecraft:nether_wastes",
        "minecraft:soul_sand_valley",
        "minecraft:crimson_forest",
        "minecraft:warped_forest",
        "minecraft:basalt_deltas",
        "minecraft:the_end",
        "minecraft:end_highlands",
        "minecraft:end_midlands",
        "minecraft:end_barrens",
        "minecraft:small_end_islands",
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
        blocks.lava = soft_id("minecraft:lava", blocks.water);
        blocks.netherrack = soft_id("minecraft:netherrack", blocks.stone);
        blocks.end_stone = soft_id("minecraft:end_stone", blocks.stone);
        blocks.deepslate = soft_id("minecraft:deepslate", blocks.stone);
        blocks.tuff = soft_id("minecraft:tuff", blocks.stone);
        blocks.granite = soft_id("minecraft:granite", blocks.stone);
        blocks.diorite = soft_id("minecraft:diorite", blocks.stone);
        blocks.andesite = soft_id("minecraft:andesite", blocks.stone);
        blocks.clay = soft_id("minecraft:clay", blocks.stone);
        blocks.snow_block = soft_id("minecraft:snow_block", blocks.snow);
        blocks.powder_snow = soft_id("minecraft:powder_snow", blocks.snow);
        blocks.packed_ice = soft_id("minecraft:packed_ice", blocks.ice);
        blocks.calcite = soft_id("minecraft:calcite", blocks.stone);
        blocks.podzol = soft_id("minecraft:podzol", blocks.dirt);
        blocks.mycelium = soft_id("minecraft:mycelium", blocks.dirt);
        blocks.mud = soft_id("minecraft:mud", blocks.dirt);
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
        // Short grass was called "minecraft:grass" before the block was renamed, and a
        // checkout that old should still grow grass instead of leaving the ground bare.
        const auto grass_plant = block("minecraft:short_grass");
        if (grass_plant) {
            blocks.short_grass = *grass_plant;
        } else {
            blocks.short_grass = soft_id("minecraft:grass", 0);
        }
        blocks.fern = soft_id("minecraft:fern", 0);
        blocks.dandelion = soft_id("minecraft:dandelion", 0);
        blocks.poppy = soft_id("minecraft:poppy", 0);
        blocks.dead_bush = soft_id("minecraft:dead_bush", 0);
        blocks.seagrass = soft_id("minecraft:seagrass", 0);
        blocks.kelp = soft_id("minecraft:kelp", 0);
        blocks.kelp_plant = soft_id("minecraft:kelp_plant", 0);
        blocks.sugar_cane = soft_id("minecraft:sugar_cane", 0);

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
            std::int32_t deep, std::int32_t log, std::int32_t leaves, int trees, int cacti) {
            const auto resolved = biome(name);
            if (!resolved) {
                LOG_WARN(Chunk, "terrain: cannot resolve biome '{}'", name);
                out.push_back(Biome{});
                complete = false;
                return;
            }
            out.push_back(Biome{ *resolved, surface, filler, deep, log, leaves, trees, cacti });
        };

        add("minecraft:deep_ocean", blocks.gravel, blocks.gravel, blocks.stone,
            0, 0, 0, 0);
        add("minecraft:ocean", blocks.gravel, blocks.gravel, blocks.stone,
            0, 0, 0, 0);
        add("minecraft:frozen_ocean", blocks.gravel, blocks.gravel, blocks.stone,
            0, 0, 0, 0);
        add("minecraft:beach", blocks.sand, blocks.sand, blocks.sandstone,
            0, 0, 0, 0);
        add("minecraft:stony_shore", blocks.stone, blocks.stone, blocks.stone,
            0, 0, 0, 0);
        add("minecraft:snowy_plains", blocks.grass, blocks.dirt, blocks.stone,
            blocks.spruce_log, blocks.spruce_leaves, 150, 0);
        add("minecraft:snowy_taiga", blocks.grass, blocks.dirt, blocks.stone,
            blocks.spruce_log, blocks.spruce_leaves, 40, 0);
        add("minecraft:taiga", blocks.grass, blocks.dirt, blocks.stone,
            blocks.spruce_log, blocks.spruce_leaves, 26, 0);
        add("minecraft:windswept_hills", blocks.grass, blocks.dirt, blocks.stone,
            blocks.spruce_log, blocks.spruce_leaves, 110, 0);
        add("minecraft:stony_peaks", blocks.stone, blocks.stone, blocks.stone,
            0, 0, 0, 0);
        add("minecraft:meadow", blocks.grass, blocks.dirt, blocks.stone,
            blocks.oak_log, blocks.oak_leaves, 200, 0);
        add("minecraft:plains", blocks.grass, blocks.dirt, blocks.stone,
            blocks.oak_log, blocks.oak_leaves, 120, 0);
        add("minecraft:forest", blocks.grass, blocks.dirt, blocks.stone,
            blocks.oak_log, blocks.oak_leaves, 18, 0);
        add("minecraft:birch_forest", blocks.grass, blocks.dirt, blocks.stone,
            blocks.birch_log, blocks.birch_leaves, 20, 0);
        add("minecraft:swamp", blocks.grass, blocks.coarse_dirt, blocks.stone,
            blocks.oak_log, blocks.oak_leaves, 70, 0);
        add("minecraft:jungle", blocks.grass, blocks.dirt, blocks.stone,
            blocks.jungle_log, blocks.jungle_leaves, 14, 0);
        add("minecraft:savanna", blocks.grass, blocks.dirt, blocks.stone,
            blocks.acacia_log, blocks.acacia_leaves, 90, 0);
        add("minecraft:badlands", blocks.red_sand, blocks.terracotta, blocks.stone,
            0, 0, 0, 60);
        add("minecraft:desert", blocks.sand, blocks.sand, blocks.sandstone,
            0, 0, 0, 40);

        // The ground cover, in the same BiomeIndex order as the table above. This is the
        // fallback a checkout without worldgen data keeps: the generator overwrites it with
        // the densities it reads from the biome's placed features.
        const auto covers = [&](std::size_t index, int chance, std::int32_t plant, int flowers,
            std::int32_t flower) {
            if (index >= out.size()) {
                return;
            }
            out[index].cover_chance = chance;
            out[index].cover = plant;
            out[index].flower_chance = flowers;
            out[index].flower = flower;
        };
        covers(kDeepOcean, 0, 0, 0, 0);
        covers(kOcean, 0, 0, 0, 0);
        covers(kFrozenOcean, 0, 0, 0, 0);
        covers(kBeach, 0, 0, 0, 0);
        covers(kStonyShore, 0, 0, 0, 0);
        covers(kSnowyPlains, 0, 0, 0, 0);
        covers(kSnowyTaiga, 0, 0, 0, 0);
        covers(kTaiga, 5, blocks.fern, 0, 0);
        covers(kWindsweptHills, 8, blocks.short_grass, 0, 0);
        covers(kStonyPeaks, 0, 0, 0, 0);
        covers(kMeadow, 2, blocks.short_grass, 7, blocks.dandelion);
        covers(kPlains, 5, blocks.short_grass, 28, blocks.dandelion);
        covers(kForest, 4, blocks.short_grass, 28, blocks.poppy);
        covers(kBirchForest, 5, blocks.short_grass, 30, blocks.dandelion);
        covers(kSwamp, 4, blocks.short_grass, 0, 0);
        covers(kJungle, 3, blocks.fern, 0, 0);
        covers(kSavanna, 4, blocks.short_grass, 40, blocks.dandelion);
        covers(kBadlands, 45, blocks.dead_bush, 0, 0);
        covers(kDesert, 28, blocks.dead_bush, 0, 0);

        // The other dimensions' biomes: nothing grows on them through this table, and a
        // checkout whose registry predates them keeps working with a placeholder id.
        const auto soft_add = [&](const char* name, std::int32_t surface, std::int32_t filler,
            std::int32_t deep) {
            const auto resolved = biome(name);
            if (!resolved) {
                LOG_DEBUG(Chunk, "terrain: biome '{}' is not in this registry", name);
                out.push_back(Biome{ 0, surface, filler, deep, 0, 0, 0, 0, false });
                return;
            }
            out.push_back(Biome{ *resolved, surface, filler, deep, 0, 0, 0, 0, false });
        };
        for (const char* name : { "minecraft:nether_wastes", "minecraft:soul_sand_valley",
                 "minecraft:crimson_forest", "minecraft:warped_forest",
                 "minecraft:basalt_deltas" }) {
            soft_add(name, blocks.netherrack, blocks.netherrack, blocks.netherrack);
        }
        for (const char* name : { "minecraft:the_end", "minecraft:end_highlands",
                 "minecraft:end_midlands", "minecraft:end_barrens",
                 "minecraft:small_end_islands" }) {
            soft_add(name, blocks.end_stone, blocks.end_stone, blocks.end_stone);
        }

        return complete && out.size() == kBiomeCount;
    }

    // What the blocks this generator places do to light. Everything else stays opaque: no
    // light passes and none is emitted.
    // Light a block a player can place gives off. The generator never puts one of these down,
    // so the terrain table above does not know them, but the overlay can hold them and their
    // light has to survive the edit, the save and the restart. Opacity and the sky flag are
    // the shape of the block: a torch is a sliver that light and the sky pass, shroomlight is
    // a full cube that keeps both out.
    inline void add_placed_light_sources(LightRules& rules, const IdResolver& block) {
        struct Source {
            const char* name;
            std::uint8_t emission;
            std::uint8_t opacity;
            bool sky_down;
        };
        static constexpr Source kSources[] = {
            { "minecraft:torch", 14, 0, true },
            { "minecraft:wall_torch", 14, 0, true },
            { "minecraft:soul_torch", 10, 0, true },
            { "minecraft:soul_wall_torch", 10, 0, true },
            { "minecraft:redstone_torch", 7, 0, true },
            { "minecraft:redstone_wall_torch", 7, 0, true },
            { "minecraft:lantern", 15, 15, false },
            { "minecraft:soul_lantern", 10, 15, false },
            { "minecraft:shroomlight", 15, 15, false },
            { "minecraft:glowstone", 15, 15, false },
            { "minecraft:sea_lantern", 15, 15, false },
            { "minecraft:jack_o_lantern", 15, 15, false },
            { "minecraft:magma_block", 3, 15, false },
            { "minecraft:lava", 15, 15, false },
            { "minecraft:campfire", 15, 15, false },
            { "minecraft:soul_campfire", 10, 15, false },
            { "minecraft:fire", 15, 0, true },
            { "minecraft:soul_fire", 10, 0, true },
            { "minecraft:glow_lichen", 7, 0, true },
            { "minecraft:amethyst_cluster", 5, 15, false },
            { "minecraft:brewing_stand", 1, 0, true },
            { "minecraft:brown_mushroom", 1, 0, true },
            { "minecraft:enchanting_table", 7, 15, false },
            { "minecraft:end_portal", 15, 15, false },
            { "minecraft:end_gateway", 15, 15, false },
            { "minecraft:crying_obsidian", 10, 15, false },
            { "minecraft:respawn_anchor", 15, 15, false },
            { "minecraft:beacon", 15, 15, false },
            { "minecraft:conduit", 15, 15, false },
        };
        for (const auto& source : kSources) {
            const auto state = block(source.name);
            if (!state) {
                continue;
            }
            rules.set(*state, LightBlock{ source.emission, source.opacity, source.sky_down });
        }
    }

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
