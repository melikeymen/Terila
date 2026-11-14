#include "item/components.hpp"

#include "log/logger.hpp"
#include "nbt/nbt.hpp"
#include "protocol/varint.hpp"

#include <algorithm>

namespace terila::item {

    std::optional<std::int32_t> Components::enchantment_level(std::string_view id) const {
        for (const auto& enchantment : enchantments) {
            if (enchantment.id == id) {
                return enchantment.level;
            }
        }
        return std::nullopt;
    }

    void Components::set_enchantment(std::string id, std::int32_t level) {
        for (auto& enchantment : enchantments) {
            if (enchantment.id == id) {
                enchantment.level = level;
                return;
            }
        }
        enchantments.push_back(Enchantment{ std::move(id), level });
    }

    ComponentIds component_ids(const std::unordered_map<std::string, std::int32_t>& registry) {
        ComponentIds ids;
        if (const auto it = registry.find("minecraft:enchantments"); it != registry.end()) {
            ids.enchantments = it->second;
        }
        if (const auto it = registry.find("minecraft:custom_name"); it != registry.end()) {
            ids.custom_name = it->second;
        }
        if (!ids.usable()) {
            LOG_WARN(Data, "data component ids are missing, items render without components");
        }
        return ids;
    }

    namespace {

        // minecraft:enchantments is a compound with a "levels" map of enchantment -> level.
        std::vector<std::uint8_t> enchantments_tag(const Components& components) {
            std::vector<std::uint8_t> tag;
            terila::nbt::begin_root(tag);
            terila::nbt::begin_compound(tag, "levels");
            for (const auto& enchantment : components.enchantments) {
                terila::nbt::write_int(tag, enchantment.id, enchantment.level);
            }
            terila::nbt::end(tag);   // levels
            terila::nbt::end(tag);   // root
            return tag;
        }

        std::vector<std::uint8_t> custom_name_tag(const std::string& name) {
            std::vector<std::uint8_t> tag;
            terila::nbt::write_text_component(tag, name);
            return tag;
        }

    } // namespace

    void write_component_patch(std::vector<std::uint8_t>& out, const Components* components,
        const ComponentIds& ids) {
        if (components == nullptr || components->empty() || !ids.usable()) {
            terila::protocol::write_varint(out, 0);
            terila::protocol::write_varint(out, 0);
            return;
        }

        struct Entry {
            std::int32_t type = -1;
            std::vector<std::uint8_t> tag;
        };

        std::vector<Entry> entries;
        if (!components->enchantments.empty() && ids.enchantments >= 0) {
            entries.push_back(Entry{ ids.enchantments, enchantments_tag(*components) });
        }
        if (!components->custom_name.empty() && ids.custom_name >= 0) {
            entries.push_back(Entry{ ids.custom_name,
                custom_name_tag(components->custom_name) });
        }

        terila::protocol::write_varint(out, static_cast<std::int32_t>(entries.size()));
        for (const auto& entry : entries) {
            terila::protocol::write_varint(out, entry.type);
            out.insert(out.end(), entry.tag.begin(), entry.tag.end());
        }
        terila::protocol::write_varint(out, 0);   // components removed from the defaults
    }

} // namespace terila::item
