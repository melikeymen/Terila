#!/usr/bin/env python3
"""Builds the mob tables from the game data and the decompiled server.

Two sources, both checked in:
  * input/entities/minecraft.json  -- width, height and category of every entity type
  * input/worldgen/biomes.json     -- what each biome spawns, with weights and pack sizes
  * <decompiled>/net/minecraft/world/entity -- the goals an entity registers and the
    attribute chain that gives it health, speed, attack damage and follow range

Nothing here runs at server start: the tables are compiled in, so a missing file can never
change how a mob behaves.

    python tools/extract_mob_data.py [--decompiled PATH]
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DECOMPILED = os.path.join(
    ROOT, "..", "..", "mc", "server", "26.3", "decompiled", "net", "minecraft")

# LivingEntity.createLivingAttributes() / Mob.createMobAttributes(): the values every mob
# starts from before its own class adds to them.
DEFAULTS = {
    "max_health": 20.0,
    "movement_speed": 0.7,
    "attack_damage": 2.0,
    "follow_range": 32.0,
    "armor": 0.0,
}

ATTRIBUTES = {
    "MAX_HEALTH": "max_health",
    "MOVEMENT_SPEED": "movement_speed",
    "ATTACK_DAMAGE": "attack_damage",
    "FOLLOW_RANGE": "follow_range",
    "ARMOR": "armor",
}

CATEGORIES = {
    "MONSTER": "Monster",
    "CREATURE": "Creature",
    "AMBIENT": "Ambient",
    "AXOLOTLS": "Axolotls",
    "UNDERGROUND_WATER_CREATURE": "UndergroundWaterCreature",
    "WATER_CREATURE": "WaterCreature",
    "WATER_AMBIENT": "WaterAmbient",
    "MISC": "Misc",
}

GOAL_CALL = re.compile(
    r"this\.(goalSelector|targetSelector)\.addGoal\(\s*(\d+)\s*,\s*new\s+([A-Za-z0-9_.]+)\s*\((.*?)\)\s*\)",
    re.S)
ATTRIBUTE_CALL = re.compile(
    r"\.add\(\s*Attributes\.([A-Z_]+)\s*(?:,\s*([-0-9.eEfF]+)\s*)?\)")
HELPER_CALL = re.compile(r"this\.([a-zA-Z0-9_]+)\(\)\s*;")

# SpawnPlacements registers where a type may appear and which light/test it needs:
#   register(EntityTypes.COW, SpawnPlacementTypes.ON_GROUND, Heightmap.Types.X,
#            Animal::checkAnimalSpawnRules);
SPAWN_REGISTER = re.compile(
    r"register\(\s*EntityTypes\.([A-Z0-9_]+)\s*,\s*SpawnPlacementTypes\.([A-Z_]+)\s*,"
    r"[^;]*?,\s*([A-Za-z0-9_]+::[A-Za-z0-9_]+)\s*\)", re.S)

PLACEMENTS = {
    "ON_GROUND": "OnGround",
    "IN_WATER": "InWater",
    "IN_LAVA": "InLava",
    "NO_RESTRICTIONS": "NoRestrictions",
}

# Predicates that test the light at the spawn position. Anything else (water animals,
# Mob::checkMobSpawnRules, nether-only mobs) is allowed at any brightness.
DARK_PREDICATES = {
    "Monster::checkMonsterSpawnRules",
    "Monster::checkSurfaceMonstersSpawnRules",
    "Bat::checkBatSpawnRules",
    "Slime::checkSlimeSpawnRules",
    "Silverfish::checkSilverfishSpawnRules",
    "Endermite::checkEndermiteSpawnRules",
    "GlowSquid::checkGlowSquidSpawnRules",
    "MagmaCube::checkMagmaCubeSpawnRules",
}
BRIGHT_PREDICATES = {
    "Animal::checkAnimalSpawnRules",
    "Armadillo::checkArmadilloSpawnRules",
    "Camel::checkCamelSpawnRules",
    "Fox::checkFoxSpawnRules",
    "Frog::checkFrogSpawnRules",
    "Goat::checkGoatSpawnRules",
    "Hoglin::checkHoglinSpawnRules",
    "MushroomCow::checkMushroomSpawnRules",
    "Ocelot::checkOcelotSpawnRules",
    "Panda::checkPandaSpawnRules",
    "Parrot::checkParrotSpawnRules",
    "PolarBear::checkPolarBearSpawnRules",
    "Rabbit::checkRabbitSpawnRules",
    "Turtle::checkTurtleSpawnRules",
    "Wolf::checkWolfSpawnRules",
}

DEFAULT_LIGHT = {"Monster": "Dark", "Creature": "Bright"}


def spawn_rules(decompiled):
    """Entity id -> (placement, light rule), straight from SpawnPlacements' static block."""
    path = os.path.join(decompiled, "world/entity/SpawnPlacements.java")
    if not os.path.isfile(path):
        return {}
    text = strip_comments(read(path))
    rules = {}
    for match in SPAWN_REGISTER.finditer(text):
        constant, placement, predicate = match.groups()
        entity_id = "minecraft:" + constant.lower()
        if predicate in DARK_PREDICATES:
            light = "Dark"
        elif predicate in BRIGHT_PREDICATES:
            light = "Bright"
        else:
            light = "Any"
        rules[entity_id] = (PLACEMENTS.get(placement, "NoRestrictions"), light)
    return rules


def read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def method_body(text, signature):
    """The body of the first method whose signature starts as given, braces balanced."""
    start = text.find(signature)
    if start < 0:
        return None
    brace = text.find("{", start)
    if brace < 0:
        return None
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1:index]
    return None


def number(token, fallback):
    if token is None:
        return fallback
    token = token.strip().rstrip("fFdD")
    try:
        return float(token)
    except ValueError:
        return fallback


def item_tags():
    """Item tag name -> the item ids it holds, from input/tags/items.json."""
    tags = json.load(open(os.path.join(ROOT, "input/tags/items.json"), encoding="utf-8"))["data"]
    by_key = {entry["id"]: entry.get("values", []) for entry in tags}

    resolved = {}

    def expand(key, seen):
        if key in resolved:
            return resolved[key]
        if key in seen:
            return []
        seen.add(key)
        items = []
        for value in by_key.get(key, []):
            if value.startswith("#"):
                items.extend(expand(value[1:], seen))
            else:
                items.append(value)
        resolved[key] = items
        return items

    for key in by_key:
        expand(key, set())
    return resolved


TAGS = {}

FOOD_TAG = re.compile(r"ItemTags\.([A-Z0-9_]+)")
FOOD_ITEM = re.compile(r"Items\.([A-Z0-9_]+)")


def entity_ids(decompiled):
    text = read(os.path.join(decompiled, "world/entity/EntityTypeIds.java"))
    names = {}
    for constant, ident in re.findall(
            r"public static final ResourceKey<EntityType<\?>> ([A-Z0-9_]+) = create\(\"([a-z0-9_]+)\"\)",
            text):
        names[constant] = "minecraft:" + ident
    return names


def entity_classes(decompiled):
    """EntityType<X> NAME = register(EntityTypeIds.K, ... MobCategory.X ...)."""
    text = strip_comments(read(os.path.join(decompiled, "world/entity/EntityTypes.java")))
    mapping = {}
    ids = entity_ids(decompiled)
    for match in re.finditer(
            r"public static final EntityType<([A-Za-z0-9_]+)> ([A-Z0-9_]+) = register\(\s*"
            r"EntityTypeIds\.([A-Z0-9_]+)(.*?)\);", text, re.S):
        cls, _constant, key, tail = match.groups()
        category = re.search(r"MobCategory\.([A-Z_]+)", tail)
        mapping[cls] = {
            "id": ids.get(key, "minecraft:" + key.lower()),
            "category": CATEGORIES.get(category.group(1) if category else "", "Misc"),
        }
    return mapping


def class_files(decompiled):
    found = {}
    for base, _dirs, files in os.walk(os.path.join(decompiled, "world/entity")):
        for name in files:
            if name.endswith(".java"):
                found.setdefault(name[:-5], []).append(os.path.join(base, name))
    return found


def parse_food(args, tags):
    """The items a TemptGoal/BreedGoal predicate names, tags expanded to their items."""
    items = []
    for tag in FOOD_TAG.findall(args):
        items.extend(tags.get("minecraft:" + tag.lower(), []))
    for item in FOOD_ITEM.findall(args):
        items.append("minecraft:" + item.lower())
    return items


def parse_class(path):
    text = strip_comments(read(path))
    parent = re.search(r"class\s+[A-Za-z0-9_]+\s+extends\s+([A-Za-z0-9_]+)", text)
    goals = []

    bodies = []
    body = method_body(text, "void registerGoals()")
    if body is not None:
        bodies.append(body)
        for helper in HELPER_CALL.findall(body):
            helper_body = method_body(text, "void " + helper + "()")
            if helper_body is not None:
                bodies.append(helper_body)
    for chunk in bodies:
        for _selector, priority, goal, args in GOAL_CALL.findall(chunk):
            target = re.search(r"([A-Za-z0-9_]+)\.class", args)
            speed = None
            for token in args.split(","):
                token = token.strip()
                if re.fullmatch(r"[-0-9.]+[fFdD]?", token):
                    speed = number(token, None)
                    break
            goals.append({
                "kind": goal.split(".")[-1],
                "priority": int(priority),
                "speed": speed,
                "target": target.group(1) if target else "",
                "food": parse_food(args, TAGS),
            })

    attributes = {}
    for signature in ("createAttributes()", "createMonsterAttributes()"):
        body = method_body(text, signature)
        if body is None:
            continue
        for constant, value in ATTRIBUTE_CALL.findall(body):
            key = ATTRIBUTES.get(constant)
            if key is not None and value:
                attributes[key] = number(value, None)
    return parent.group(1) if parent else None, goals, attributes


def write_mobs(out, decompiled):
    entities = json.load(open(os.path.join(ROOT, "input/entities/minecraft.json"),
                              encoding="utf-8"))["data"]
    by_id = {entry["id"]: entry for entry in entities}
    classes = entity_classes(DEFAULT_DECOMPILED)
    files = class_files(DEFAULT_DECOMPILED)
    parsed = {}

    def info(cls):
        if cls not in parsed:
            path = files.get(cls, [None])[0]
            parsed[cls] = (None, [], {}) if path is None else parse_class(path)
        return parsed[cls]

    rows = []
    for cls, meta in sorted(classes.items(), key=lambda item: item[1]["id"]):
        data = by_id.get(meta["id"])
        if data is None or meta["category"] == "Misc":
            continue

        goals = {}
        attributes = dict(DEFAULTS)
        seen = set()
        current = cls
        while current and current not in seen and current not in ("Mob", "LivingEntity", "Entity"):
            seen.add(current)
            parent, class_goals, class_attributes = info(current)
            for goal in class_goals:
                goals.setdefault((goal["kind"], goal["priority"]), goal)
            for key, value in class_attributes.items():
                if value is not None:
                    attributes[key] = value
            current = parent

        rows.append({
            "id": meta["id"],
            "category": meta["category"],
            "width": data["width"],
            "height": data["height"],
            "attributes": attributes,
            "goals": [goals[key] for key in sorted(goals, key=lambda k: (k[1], k[0]))],
        })

    rules = spawn_rules(decompiled)
    for row in rows:
        placement, light = rules.get(row["id"], (None, None))
        if placement is None:
            placement = "NoRestrictions"
        if light is None:
            light = DEFAULT_LIGHT.get(row["category"], "Any")
        row["placement"] = placement
        row["light"] = light

    rows.sort(key=lambda row: row["id"])

    # What tempts and breeds a mob: the items its TemptGoal/BreedGoal name, with item tags
    # expanded through input/tags/items.json.
    food_lines = []
    for row in rows:
        items = []
        for goal in row["goals"]:
            if goal["kind"] in ("TemptGoal", "BreedGoal"):
                for item in goal["food"]:
                    if item not in items:
                        items.append(item)
        row["food_begin"] = len(food_lines)
        for item in items:
            food_lines.append('        "%s",' % item)
        row["food_count"] = len(food_lines) - row["food_begin"]

    goal_lines = []
    for row in rows:
        row["goal_begin"] = len(goal_lines)
        for goal in row["goals"]:
            speed = goal["speed"] if goal["speed"] is not None else 1.0
            goal_lines.append('        { "%s", %d, %.3ff, "%s" },'
                              % (goal["kind"], goal["priority"], speed, goal["target"]))
        row["goal_count"] = len(goal_lines) - row["goal_begin"]

    lines = [
        "#pragma once",
        "",
        "// Generated by tools/extract_mob_data.py - do not edit by hand.",
        "//",
        "// One row per spawnable entity type: size and category from",
        "// input/entities/minecraft.json, and the attribute chain and goal list the decompiled",
        "// class hierarchy registers for it.",
        "",
        "#include <cstdint>",
        "#include <string>",
        "#include <string_view>",
        "",
        "namespace terila::entity::data {",
        "",
        "    enum class MobCategory : std::uint8_t {",
        "        Monster,",
        "        Creature,",
        "        Ambient,",
        "        Axolotls,",
        "        UndergroundWaterCreature,",
        "        WaterCreature,",
        "        WaterAmbient,",
        "        Misc,",
        "    };",
        "",
        "    struct GoalSpec {",
        '        std::string_view kind;       // vanilla goal class, e.g. "MeleeAttackGoal"',
        "        std::uint8_t priority = 0;   // lower runs first",
        "        float speed = 1.0f;          // speed modifier the goal was built with",
        '        std::string_view target;     // class the goal targets, e.g. "Player"',
        "    };",
        "",
        "    // Where the type may appear (SpawnPlacementTypes) and the light it needs",
        "    // (the checkSpawnRules predicate SpawnPlacements registers for it).",
        "    enum class SpawnPlacement : std::uint8_t {",
        "        NoRestrictions,",
        "        OnGround,",
        "        InWater,",
        "        InLava,",
        "    };",
        "",
        "    enum class SpawnLight : std::uint8_t {",
        "        Any,",
        "        Dark,",
        "        Bright,",
        "    };",
        "",
        "    struct MobSpec {",
        "        std::string_view id;",
        "        MobCategory category = MobCategory::Misc;",
        "        float width = 0.6f;",
        "        float height = 1.8f;",
        "        float max_health = 20.0f;",
        "        float movement_speed = 0.7f;",
        "        float attack_damage = 2.0f;",
        "        float follow_range = 32.0f;",
        "        float armor = 0.0f;",
        "        SpawnPlacement placement = SpawnPlacement::NoRestrictions;",
        "        SpawnLight light = SpawnLight::Any;",
        "        std::uint16_t goal_begin = 0;",
        "        std::uint16_t goal_count = 0;",
        "        std::uint16_t food_begin = 0;",
        "        std::uint16_t food_count = 0;",
        "    };",
        "",
        "    inline constexpr GoalSpec kGoals[] = {",
    ]
    lines.extend(goal_lines if goal_lines else ["        {},"])
    lines.append("    };")
    lines.append("")
    lines.append("    // The items a mob eats: what it follows in Tempt and what breeds it.")
    lines.append("    inline constexpr std::string_view kFoodItems[] = {")
    lines.extend(food_lines if food_lines else ["        {},"])
    lines.append("    };")
    lines.append("")
    lines.append("    inline constexpr MobSpec kMobs[] = {")
    for row in rows:
        attributes = row["attributes"]
        lines.append(
            '        { "%s", MobCategory::%s, %.3ff, %.3ff, %.1ff, %.3ff, %.2ff, %.1ff, %.1ff, '
            'SpawnPlacement::%s, SpawnLight::%s, %d, %d, %d, %d },'
            % (row["id"], row["category"], row["width"], row["height"],
               attributes["max_health"], attributes["movement_speed"],
               attributes["attack_damage"], attributes["follow_range"],
               attributes["armor"], row["placement"], row["light"],
               row["goal_begin"], row["goal_count"],
               row["food_begin"], row["food_count"]))
    lines.append("    };")
    lines.append("")
    lines.append("    // Whether a mob of this spec eats the given item.")
    lines.append("    inline bool eats(const MobSpec& mob, std::string_view item) {")
    lines.append("        for (int i = 0; i < mob.food_count; ++i) {")
    lines.append("            if (kFoodItems[mob.food_begin + i] == item) {")
    lines.append("                return true;")
    lines.append("            }")
    lines.append("        }")
    lines.append("        return false;")
    lines.append("    }")
    lines.append("")
    lines.append("    inline const MobSpec* find_mob(std::string_view id) {")
    lines.append("        for (const auto& mob : kMobs) {")
    lines.append("            if (mob.id == id) {")
    lines.append("                return &mob;")
    lines.append("            }")
    lines.append("        }")
    lines.append("        return nullptr;")
    lines.append("    }")
    lines.append("")
    lines.append("    inline const MobSpec* find_mob_by_name(const std::string& name) {")
    lines.append("        const std::string key =")
    lines.append("            name.find(':') == std::string::npos ? \"minecraft:\" + name : name;")
    lines.append("        return find_mob(key);")
    lines.append("    }")
    lines.append("")
    lines.append("} // namespace terila::entity::data")
    lines.append("")

    with open(out, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    print("wrote %s: %d mobs, %d goals" % (out, len(rows), len(goal_lines)))


def category_key(kind):
    """The lowercase snake_case key the C++ spawner matches (water_creature, ...)."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", kind).lower()


def spawn_count(value):
    if isinstance(value, dict):
        return int(value.get("min", 1)), int(value.get("max", 1))
    return int(value), int(value)


def write_biome_spawns(out):
    biomes = json.load(open(os.path.join(ROOT, "input/worldgen/biomes.json"),
                            encoding="utf-8"))["data"]
    entries = []
    rows = []
    for biome in biomes:
        spawns = biome.get("data", {}).get("attributes", {}).get(
            "minecraft:gameplay/natural_mob_spawns", {}).get("argument", {}).get(
            "spawns_by_category", {})
        begin = len(entries)
        for category, list_ in spawns.items():
            kind = CATEGORIES.get(category.upper())
            if kind is None or kind == "Misc":
                continue
            for entry in list_:
                low, high = spawn_count(entry.get("count", 1))
                entries.append((entry["type"], kind, int(entry.get("weight", 1)), low, high))
        if len(entries) > begin:
            rows.append((biome["id"], begin, len(entries) - begin))

    lines = [
        "#pragma once",
        "",
        "// Generated by tools/extract_mob_data.py - do not edit by hand.",
        "//",
        "// input/worldgen/biomes.json: the natural spawn list of every biome, weights and",
        "// pack sizes included, which is what the natural spawner rolls against.",
        "",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace terila::entity::data {",
        "",
        "    struct BiomeSpawn {",
        "        std::string_view type;",
        "        std::string_view category;",
        "        std::uint16_t weight = 0;",
        "        std::uint8_t min_count = 1;",
        "        std::uint8_t max_count = 1;",
        "    };",
        "",
        "    struct BiomeSpawnList {",
        "        std::string_view id;",
        "        std::uint16_t begin = 0;",
        "        std::uint16_t count = 0;",
        "    };",
        "",
        "    inline constexpr BiomeSpawn kBiomeSpawns[] = {",
    ]
    for type_, kind, weight, low, high in entries:
        lines.append('        { "%s", "%s", %d, %d, %d },'
                     % (type_, category_key(kind), weight, low, high))
    lines.append("    };")
    lines.append("")
    lines.append("    inline constexpr BiomeSpawnList kBiomeSpawnLists[] = {")
    for ident, begin, count in rows:
        lines.append('        { "%s", %d, %d },' % (ident, begin, count))
    lines.append("    };")
    lines.append("")
    lines.append("    inline const BiomeSpawnList* find_biome_spawns(std::string_view id) {")
    lines.append("        for (const auto& biome : kBiomeSpawnLists) {")
    lines.append("            if (biome.id == id) {")
    lines.append("                return &biome;")
    lines.append("            }")
    lines.append("        }")
    lines.append("        return nullptr;")
    lines.append("    }")
    lines.append("")
    lines.append("} // namespace terila::entity::data")
    lines.append("")

    with open(out, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    print("wrote %s: %d biomes, %d spawn entries" % (out, len(rows), len(entries)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decompiled", default=DEFAULT_DECOMPILED)
    parser.add_argument("--out", default=os.path.join(ROOT, "src/entity/mob_data.hpp"))
    parser.add_argument("--spawns", default=os.path.join(ROOT, "src/world/biome_spawns.hpp"))
    args = parser.parse_args()

    if not os.path.isdir(args.decompiled):
        sys.exit("no decompiled tree at " + args.decompiled)

    global TAGS
    TAGS = item_tags()

    write_mobs(args.out, args.decompiled)
    write_biome_spawns(args.spawns)


if __name__ == "__main__":
    main()
