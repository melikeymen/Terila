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

        bool empty() const { return enchantments.empty() && custom_name.empty(); }
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

    // Registry ids of the component types this build writes.
    struct ComponentIds {
        std::int32_t enchantments = -1;   // minecraft:enchantments
        std::int32_t custom_name = -1;    // minecraft:custom_name
        const std::unordered_map<std::string, std::int32_t>* enchantment_registry = nullptr;

        bool usable() const { return enchantments >= 0 || custom_name >= 0; }
    };

    ComponentIds component_ids(const std::unordered_map<std::string, std::int32_t>& registry);

    // Appends the component patch that follows the item id: the added component count,
    // each component as (type id, NBT value), then the removed count.
    void write_component_patch(std::vector<std::uint8_t>& out, const Components* components,
        const ComponentIds& ids);

} // namespace terila::item
