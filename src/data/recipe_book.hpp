#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace terila::data {

    // One slot of a recipe: the items it accepts. Tag ingredients are expanded at load
    // time, so matching is a membership test.
    struct Ingredient {
        std::vector<std::int32_t> items;

        bool empty() const { return items.empty(); }
        bool accepts(std::int32_t item) const;
    };

    struct ShapedRecipe {
        int width = 0;
        int height = 0;
        std::vector<Ingredient> grid;   // width * height, row major
        std::int32_t result_item = 0;
        std::int32_t result_count = 1;
    };

    struct ShapelessRecipe {
        std::vector<Ingredient> ingredients;
        std::int32_t result_item = 0;
        std::int32_t result_count = 1;
    };

    struct CookingRecipe {
        Ingredient input;
        std::int32_t result_item = 0;
        std::int32_t result_count = 1;
        int cook_ticks = 200;
    };

    struct CraftResult {
        std::int32_t item_id = 0;
        std::int32_t count = 1;
    };

    // The crafting table and furnace recipes from input/recipes, keyed by item ids.
    class RecipeBook {
    public:
        // Both paths are relative to the input root and are read through the input store.
        bool load(const std::string& recipes_path, const std::string& item_tags_path,
            const std::unordered_map<std::string, std::int32_t>& item_ids,
            const std::unordered_map<std::string, std::int32_t>& block_state_ids);

        // grid holds nine item ids (3x3, row major) and returns the crafted stack.
        std::optional<CraftResult> match_crafting(const std::array<std::int32_t, 9>& grid) const;

        // Smelting family lookup for a furnace input slot.
        std::optional<CookingRecipe> find_cooking(std::int32_t item_id) const;

        // Ticks of burn time an item provides as furnace fuel, zero when it is not fuel.
        int fuel_ticks(std::int32_t item_id) const;

        std::size_t crafting_count() const { return shaped_.size() + shapeless_.size(); }
        std::size_t cooking_count() const { return cooking_.size(); }
        std::size_t fuel_count() const { return fuel_.size(); }

    private:
        std::vector<ShapedRecipe> shaped_;
        std::vector<ShapelessRecipe> shapeless_;
        std::vector<CookingRecipe> cooking_;
        std::unordered_map<std::int32_t, int> fuel_;
    };

} // namespace terila::data
