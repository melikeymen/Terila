#include "data/recipe_book.hpp"

#include "log/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>

namespace terila::data {

    namespace {

        using json = nlohmann::json;

        bool contains(const std::vector<std::int32_t>& items, std::int32_t item) {
            return std::find(items.begin(), items.end(), item) != items.end();
        }

        // A recipe ingredient is either one id, a list of ids or a "#tag" reference.
        Ingredient read_ingredient(const json& value,
            const std::unordered_map<std::string, std::int32_t>& item_ids,
            const std::unordered_map<std::string, std::vector<std::int32_t>>& tags) {

            Ingredient ingredient;

            const auto add = [&](const std::string& id) {
                if (!id.empty() && id[0] == '#') {
                    const auto tag = tags.find(id.substr(1));
                    if (tag == tags.end()) {
                        return;
                    }
                    for (const auto entry : tag->second) {
                        if (!contains(ingredient.items, entry)) {
                            ingredient.items.push_back(entry);
                        }
                    }
                    return;
                }
                const auto item = item_ids.find(id);
                if (item != item_ids.end() && !contains(ingredient.items, item->second)) {
                    ingredient.items.push_back(item->second);
                }
            };

            if (value.is_string()) {
                add(value.get<std::string>());
            }
            else if (value.is_array()) {
                for (const auto& entry : value) {
                    if (entry.is_string()) {
                        add(entry.get<std::string>());
                    }
                }
            }

            return ingredient;
        }

        std::int32_t read_item(const json& value,
            const std::unordered_map<std::string, std::int32_t>& item_ids) {
            if (!value.is_object() || !value.contains("id")) {
                return 0;
            }
            const auto it = item_ids.find(value["id"].get<std::string>());
            return it == item_ids.end() ? 0 : it->second;
        }

        std::int32_t read_count(const json& value) {
            if (value.is_object() && value.contains("count")) {
                return value["count"].get<std::int32_t>();
            }
            return 1;
        }

    } // namespace

    bool Ingredient::accepts(std::int32_t item) const {
        return item != 0 && contains(items, item);
    }

    bool RecipeBook::load(const std::string& recipes_path, const std::string& item_tags_path,
        const std::unordered_map<std::string, std::int32_t>& item_ids,
        const std::unordered_map<std::string, std::int32_t>& block_state_ids) {

        // Item tags first: recipes refer to "#minecraft:planks" and friends.
        std::unordered_map<std::string, std::vector<std::string>> raw_tags;

        std::ifstream tag_file(item_tags_path);
        if (tag_file) {
            try {
                const auto tags = json::parse(tag_file);
                for (const auto& row : tags["data"]) {
                    std::vector<std::string> values;
                    for (const auto& value : row["values"]) {
                        if (value.is_string()) {
                            values.push_back(value.get<std::string>());
                        }
                    }
                    raw_tags[row["id"].get<std::string>()] = std::move(values);
                }
            }
            catch (const std::exception& e) {
                LOG_WARN(Data, "cannot parse item tags '{}': {}", item_tags_path, e.what());
            }
        }

        std::unordered_map<std::string, std::vector<std::int32_t>> tags;
        for (const auto& [name, _] : raw_tags) {
            std::vector<std::int32_t> items;
            std::vector<std::string> pending{ name };
            int depth = 0;

            while (!pending.empty() && depth < 8) {
                std::vector<std::string> next;
                for (const auto& tag_name : pending) {
                    const auto row = raw_tags.find(tag_name);
                    if (row == raw_tags.end()) {
                        continue;
                    }
                    for (const auto& value : row->second) {
                        if (!value.empty() && value[0] == '#') {
                            next.push_back(value.substr(1));
                            continue;
                        }
                        const auto item = item_ids.find(value);
                        if (item != item_ids.end() && !contains(items, item->second)) {
                            items.push_back(item->second);
                        }
                    }
                }
                pending = std::move(next);
                ++depth;
            }

            tags[name] = std::move(items);
        }

        std::ifstream file(recipes_path);
        if (!file) {
            LOG_ERROR(Data, "cannot open recipes '{}'", recipes_path);
            return false;
        }

        try {
            const auto root = json::parse(file);
            for (const auto& row : root["data"]) {
                const std::string type = row.value("type", "");
                const auto& recipe = row["recipe"];
                const std::string recipe_type = recipe.value("type", "");

                if (type == "minecraft:crafting" && recipe_type == "minecraft:crafting_shaped") {
                    ShapedRecipe shaped;
                    const auto& pattern = recipe["pattern"];
                    shaped.height = static_cast<int>(pattern.size());
                    for (const auto& line : pattern) {
                        shaped.width = std::max(shaped.width,
                            static_cast<int>(line.get<std::string>().size()));
                    }

                    const auto& key = recipe["key"];
                    std::unordered_map<char, Ingredient> by_char;
                    for (auto it = key.begin(); it != key.end(); ++it) {
                        by_char[it.key()[0]] = read_ingredient(it.value(), item_ids, tags);
                    }

                    shaped.grid.resize(static_cast<std::size_t>(shaped.width) *
                        static_cast<std::size_t>(shaped.height));
                    for (int y = 0; y < shaped.height; ++y) {
                        const std::string line = pattern[y].get<std::string>();
                        for (int x = 0; x < shaped.width; ++x) {
                            const char symbol = x < static_cast<int>(line.size()) ? line[x] : ' ';
                            const auto entry = by_char.find(symbol);
                            if (entry != by_char.end()) {
                                shaped.grid[static_cast<std::size_t>(y * shaped.width + x)] =
                                    entry->second;
                            }
                        }
                    }

                    shaped.result_item = read_item(recipe["result"], item_ids);
                    shaped.result_count = read_count(recipe["result"]);
                    if (shaped.result_item != 0 && shaped.width > 0) {
                        shaped_.push_back(std::move(shaped));
                    }
                }
                else if (type == "minecraft:crafting" &&
                    recipe_type == "minecraft:crafting_shapeless") {
                    ShapelessRecipe shapeless;
                    for (const auto& entry : recipe["ingredients"]) {
                        shapeless.ingredients.push_back(
                            read_ingredient(entry, item_ids, tags));
                    }
                    shapeless.result_item = read_item(recipe["result"], item_ids);
                    shapeless.result_count = read_count(recipe["result"]);
                    if (shapeless.result_item != 0 && !shapeless.ingredients.empty()) {
                        shapeless_.push_back(std::move(shapeless));
                    }
                }
                else if (type == "minecraft:smelting" || type == "minecraft:blasting" ||
                    type == "minecraft:smoking" || type == "minecraft:campfire_cooking") {
                    CookingRecipe cooking;
                    cooking.input = read_ingredient(recipe["ingredient"], item_ids, tags);
                    cooking.result_item = read_item(recipe["result"], item_ids);
                    cooking.result_count = read_count(recipe["result"]);
                    cooking.cook_ticks = recipe.value("cookingtime", 200);
                    if (cooking.result_item != 0 && !cooking.input.empty()) {
                        cooking_.push_back(std::move(cooking));
                    }
                }
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR(Data, "cannot parse recipes '{}': {}", recipes_path, e.what());
            return false;
        }

        // A small fuel table: what a furnace accepts and for how long it burns.
        struct FuelEntry {
            const char* name;
            int ticks;
        };
        static const FuelEntry kFuels[] = {
            { "minecraft:coal", 1600 },
            { "minecraft:charcoal", 1600 },
            { "minecraft:coal_block", 16000 },
            { "minecraft:stick", 100 },
            { "minecraft:lava_bucket", 20000 },
            { "minecraft:blaze_rod", 2400 },
            { "minecraft:dried_kelp_block", 4000 },
            { "minecraft:oak_planks", 300 },
            { "minecraft:spruce_planks", 300 },
            { "minecraft:birch_planks", 300 },
            { "minecraft:jungle_planks", 300 },
            { "minecraft:acacia_planks", 300 },
            { "minecraft:dark_oak_planks", 300 },
            { "minecraft:oak_log", 300 },
            { "minecraft:spruce_log", 300 },
            { "minecraft:birch_log", 300 },
            { "minecraft:jungle_log", 300 },
            { "minecraft:acacia_log", 300 },
            { "minecraft:dark_oak_log", 300 },
            { "minecraft:crafting_table", 300 },
        };
        for (const auto& fuel : kFuels) {
            const auto item = item_ids.find(fuel.name);
            if (item != item_ids.end()) {
                fuel_[item->second] = fuel.ticks;
            }
        }

        (void)block_state_ids;
        LOG_INFO(Data, "recipes loaded: {} crafting, {} cooking, {} fuels",
            crafting_count(), cooking_count(), fuel_.size());
        return !shaped_.empty() || !shapeless_.empty();
    }

    std::optional<CraftResult> RecipeBook::match_crafting(
        const std::array<std::int32_t, 9>& grid) const {

        // Bounding box of the items in the crafting grid, so a recipe placed anywhere in
        // the 3x3 area still matches.
        int min_x = 3, min_y = 3, max_x = -1, max_y = -1;
        int filled = 0;
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                if (grid[static_cast<std::size_t>(y * 3 + x)] == 0) {
                    continue;
                }
                ++filled;
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
            }
        }
        if (filled == 0) {
            return std::nullopt;
        }

        const int width = max_x - min_x + 1;
        const int height = max_y - min_y + 1;

        for (const auto& recipe : shaped_) {
            if (recipe.width != width || recipe.height != height) {
                continue;
            }

            bool matches = true;
            for (int y = 0; y < height && matches; ++y) {
                for (int x = 0; x < width; ++x) {
                    const auto& ingredient =
                        recipe.grid[static_cast<std::size_t>(y * width + x)];
                    const std::int32_t item =
                        grid[static_cast<std::size_t>((y + min_y) * 3 + (x + min_x))];

                    if (ingredient.empty()) {
                        if (item != 0) {
                            matches = false;
                            break;
                        }
                        continue;
                    }
                    if (!ingredient.accepts(item)) {
                        matches = false;
                        break;
                    }
                }
            }

            if (matches) {
                return CraftResult{ recipe.result_item, recipe.result_count };
            }
        }

        for (const auto& recipe : shapeless_) {
            if (static_cast<int>(recipe.ingredients.size()) != filled) {
                continue;
            }

            std::vector<bool> used(recipe.ingredients.size(), false);
            bool matches = true;

            for (int y = 0; y < 3 && matches; ++y) {
                for (int x = 0; x < 3 && matches; ++x) {
                    const std::int32_t item = grid[static_cast<std::size_t>(y * 3 + x)];
                    if (item == 0) {
                        continue;
                    }

                    bool placed = false;
                    for (std::size_t i = 0; i < recipe.ingredients.size(); ++i) {
                        if (used[i] || !recipe.ingredients[i].accepts(item)) {
                            continue;
                        }
                        used[i] = true;
                        placed = true;
                        break;
                    }
                    if (!placed) {
                        matches = false;
                    }
                }
            }

            if (matches) {
                return CraftResult{ recipe.result_item, recipe.result_count };
            }
        }

        return std::nullopt;
    }

    std::optional<CookingRecipe> RecipeBook::find_cooking(std::int32_t item_id) const {
        for (const auto& recipe : cooking_) {
            if (recipe.input.accepts(item_id)) {
                return recipe;
            }
        }
        return std::nullopt;
    }

    int RecipeBook::fuel_ticks(std::int32_t item_id) const {
        const auto it = fuel_.find(item_id);
        return it == fuel_.end() ? 0 : it->second;
    }

} // namespace terila::data
