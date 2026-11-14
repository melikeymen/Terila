#include "item/components.hpp"

#include "log/logger.hpp"
#include "nbt/nbt_writer.hpp"
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
            terila::nbt::Writer writer(terila::nbt::StringFormat::Network);
            writer.begin_root_compound();
            writer.begin_compound("levels");
            for (const auto& enchantment : components.enchantments) {
                writer.write_int(enchantment.id, enchantment.level);
            }
            writer.end_compound();
            writer.end_compound();
            return writer.finish();
        }

        std::vector<std::uint8_t> custom_name_tag(const std::string& name) {
            terila::nbt::Writer writer(terila::nbt::StringFormat::Network);
            writer.begin_root_compound();
            writer.write_string("text", name);
            writer.end_compound();
            return writer.finish();
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
