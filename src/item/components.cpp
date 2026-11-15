#include "item/components.hpp"

#include "log/logger.hpp"
#include "tag/text.hpp"
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

    std::int32_t ComponentIds::find(std::string_view name) const {
        if (registry == nullptr) {
            return -1;
        }
        const auto it = registry->find(std::string(name));
        return it == registry->end() ? -1 : it->second;
    }

    ComponentIds component_ids(const std::unordered_map<std::string, std::int32_t>& registry,
        const std::unordered_map<std::string, std::int32_t>* enchantment_registry) {
        ComponentIds ids;
        ids.registry = &registry;
        ids.enchantment_registry = enchantment_registry;
        ids.enchantments = ids.find("minecraft:enchantments");
        ids.custom_name = ids.find("minecraft:custom_name");
        ids.item_name = ids.find("minecraft:item_name");
        ids.damage = ids.find("minecraft:damage");
        ids.max_stack_size = ids.find("minecraft:max_stack_size");
        ids.unbreakable = ids.find("minecraft:unbreakable");
        if (!ids.usable()) {
            LOG_WARN(Data, "data component ids are missing, items render without components");
        }
        return ids;
    }

    namespace {

        // A component value that is a chat component travels as network NBT, so a plain
        // name ("Hi") is wrapped the way the client expects to read it back.
        void write_component_nbt(std::vector<std::uint8_t>& out, const std::string& text) {
            const auto bytes = terila::tag::text_component_bytes(text);
            out.insert(out.end(), bytes.begin(), bytes.end());
        }

    } // namespace

    void write_component_patch(std::vector<std::uint8_t>& out, const Components* components,
        const ComponentIds& ids) {
        if (components == nullptr || components->empty() || !ids.usable()) {
            terila::protocol::write_varint(out, 0);
            terila::protocol::write_varint(out, 0);
            return;
        }

        std::vector<std::pair<std::int32_t, std::vector<std::uint8_t>>> entries;
        std::vector<std::int32_t> removals;
        for (const auto removal : components->removed) {
            if (removal >= 0) {
                removals.push_back(removal);
            }
        }

        if ((components->enchantments_present || !components->enchantments.empty()) &&
            ids.enchantments >= 0) {
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
        if (!components->custom_name_nbt.empty() && ids.custom_name >= 0) {
            entries.push_back({ ids.custom_name, components->custom_name_nbt });
        }
        else if (!components->custom_name.empty() && ids.custom_name >= 0) {
            std::vector<std::uint8_t> payload;
            write_component_nbt(payload, components->custom_name);
            entries.push_back({ ids.custom_name, std::move(payload) });
        }
        if (!components->item_name_nbt.empty() && ids.item_name >= 0) {
            entries.push_back({ ids.item_name, components->item_name_nbt });
        }
        else if (!components->item_name.empty() && ids.item_name >= 0) {
            std::vector<std::uint8_t> payload;
            write_component_nbt(payload, components->item_name);
            entries.push_back({ ids.item_name, std::move(payload) });
        }
        // Both of these are plain varints on the wire (ByteBufCodecs.VAR_INT).
        if (components->damage.has_value() && ids.damage >= 0) {
            std::vector<std::uint8_t> payload;
            terila::protocol::write_varint(payload, *components->damage);
            entries.push_back({ ids.damage, std::move(payload) });
        }
        if (components->max_stack_size.has_value() && ids.max_stack_size >= 0) {
            std::vector<std::uint8_t> payload;
            terila::protocol::write_varint(payload, *components->max_stack_size);
            entries.push_back({ ids.max_stack_size, std::move(payload) });
        }
        // Unit.STREAM_CODEC writes nothing at all, so the component is just its id.
        if (components->unbreakable && ids.unbreakable >= 0) {
            entries.push_back({ ids.unbreakable, {} });
        }

        terila::protocol::write_varint(out, static_cast<std::int32_t>(entries.size()));
        terila::protocol::write_varint(out, static_cast<std::int32_t>(removals.size()));
        for (const auto& [type_id, tag] : entries) {
            terila::protocol::write_varint(out, type_id);
            out.insert(out.end(), tag.begin(), tag.end());
        }
        for (const auto removal : removals) {
            terila::protocol::write_varint(out, removal);
        }
    }

} // namespace terila::item
