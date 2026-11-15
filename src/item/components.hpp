#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace terila::item {

    struct Enchantment {
        std::string id;          // "minecraft:efficiency"
        std::int32_t level = 1;  // 255 is legal: the component carries no cap

        bool operator==(const Enchantment&) const = default;
    };

    // Everything the client needs to render a stack beyond its item id. This is the
    // data component patch 1.20.5+ sends with every slot.
    struct Components {
        std::vector<Enchantment> enchantments;
        std::string custom_name;
        std::string item_name;
        // A full chat component from the command keeps its own network NBT; the plain
        // strings above are the shorthand (custom_name="Hi") wrapped as {"text": ...}.
        std::vector<std::uint8_t> custom_name_nbt;
        std::vector<std::uint8_t> item_name_nbt;
        std::optional<std::int32_t> damage;
        std::optional<std::int32_t> max_stack_size;
        bool unbreakable = false;
        // Set even when the list is empty: /give ...[minecraft:enchantments={levels:{}}]
        // has to send the empty map, which clears the item's own enchantments.
        bool enchantments_present = false;
        // `!minecraft:unbreakable` in the command: the component is removed from the
        // item's defaults instead of being set.
        std::vector<std::int32_t> removed;

        bool empty() const {
            return enchantments.empty() && !enchantments_present && custom_name.empty() &&
                item_name.empty() && custom_name_nbt.empty() && item_name_nbt.empty() &&
                !damage.has_value() && !max_stack_size.has_value() && !unbreakable &&
                removed.empty();
        }
        std::optional<std::int32_t> enchantment_level(std::string_view id) const;
        void set_enchantment(std::string id, std::int32_t level);

        // Two stacks only merge when their components are identical.
        bool operator==(const Components&) const = default;
    };

    using ComponentsPtr = std::shared_ptr<const Components>;

    // A null patch and an empty patch both mean "the item's default components", so
    // they have to stack together: compare the values, not the shared pointers.
    inline bool same_components(const ComponentsPtr& lhs, const ComponentsPtr& rhs) {
        if (lhs == rhs) {
            return true;
        }
        if (lhs == nullptr) {
            return rhs->empty();
        }
        if (rhs == nullptr) {
            return lhs->empty();
        }
        return *lhs == *rhs;
    }

    // Registry ids of the component types this build writes, plus the registry itself so
    // a name from the command can be resolved.
    struct ComponentIds {
        std::int32_t enchantments = -1;      // minecraft:enchantments
        std::int32_t custom_name = -1;       // minecraft:custom_name
        std::int32_t item_name = -1;         // minecraft:item_name
        std::int32_t damage = -1;            // minecraft:damage
        std::int32_t max_stack_size = -1;    // minecraft:max_stack_size
        std::int32_t unbreakable = -1;       // minecraft:unbreakable
        const std::unordered_map<std::string, std::int32_t>* registry = nullptr;
        const std::unordered_map<std::string, std::int32_t>* enchantment_registry = nullptr;

        bool usable() const { return enchantments >= 0 || custom_name >= 0; }
        std::int32_t find(std::string_view name) const;
    };

    ComponentIds component_ids(const std::unordered_map<std::string, std::int32_t>& registry,
        const std::unordered_map<std::string, std::int32_t>* enchantment_registry = nullptr);

    // Appends the component patch that follows the item id: added count, removed count,
    // then each added component as a type id and its encoded value.
    void write_component_patch(std::vector<std::uint8_t>& out, const Components* components,
        const ComponentIds& ids);

} // namespace terila::item
