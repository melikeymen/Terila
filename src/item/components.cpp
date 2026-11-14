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

    ComponentIds component_ids(const std::unordered_map<std::string, std::int32_t>& registry,
        const std::unordered_map<std::string, std::int32_t>* enchantment_registry) {
        ComponentIds ids;
        ids.enchantment_registry = enchantment_registry;
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

    void write_component_patch(std::vector<std::uint8_t>& out, const Components* components,
        const ComponentIds& ids) {
        if (components == nullptr || components->empty() || !ids.usable()) {
            terila::protocol::write_varint(out, 0);
            terila::protocol::write_varint(out, 0);
            return;
        }

        std::vector<std::pair<std::int32_t, std::vector<std::uint8_t>>> entries;
        if (!components->enchantments.empty() && ids.enchantments >= 0) {
            std::vector<std::uint8_t> payload;
            terila::protocol::write_varint(payload,
                static_cast<std::int32_t>(components->enchantments.size()));
            for (const auto& e : components->enchantments) {
                std::int32_t enchantment_id = 0;
                if (ids.enchantment_registry != nullptr) {
                    const auto it = ids.enchantment_registry->find(e.id);
                    if (it != ids.enchantment_registry->end()) {
                        enchantment_id = it->second;
                    }
                }
                terila::protocol::write_varint(payload, enchantment_id);
                terila::protocol::write_varint(payload, e.level);
            }
            entries.push_back({ ids.enchantments, std::move(payload) });
        }
        if (!components->custom_name.empty() && ids.custom_name >= 0) {
            terila::nbt::Writer writer(terila::nbt::StringFormat::Network);
            writer.begin_root_compound();
            writer.write_string("text", components->custom_name);
            writer.end_compound();
            entries.push_back({ ids.custom_name, writer.finish() });
        }

        terila::protocol::write_varint(out, static_cast<std::int32_t>(entries.size()));
        terila::protocol::write_varint(out, 0);
        for (const auto& [type_id, tag] : entries) {
            terila::protocol::write_varint(out, type_id);
            out.insert(out.end(), tag.begin(), tag.end());
        }
    }

} // namespace terila::item
