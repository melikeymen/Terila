#include "item/item_parser.hpp"

#include "nbt/nbt_writer.hpp"
#include "nbt/snbt.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace terila::item {

    namespace {

        bool is_space(char c) {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        std::string lower(std::string value) {
            for (char& c : value) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return value;
        }

        std::string_view trim(std::string_view text) {
            while (!text.empty() && is_space(text.front())) {
                text.remove_prefix(1);
            }
            while (!text.empty() && is_space(text.back())) {
                text.remove_suffix(1);
            }
            return text;
        }

        // Splits on a separator that is not inside braces, brackets or a quoted string, so
        // levels:{"minecraft:sharpness":5} stays one piece.
        std::vector<std::string_view> split_top_level(std::string_view text, char separator) {
            std::vector<std::string_view> parts;
            int depth = 0;
            char quote = 0;
            std::size_t start = 0;
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (quote != 0) {
                    if (c == '\\') {
                        ++i;
                    }
                    else if (c == quote) {
                        quote = 0;
                    }
                    continue;
                }
                if (c == '"' || c == '\'') {
                    quote = c;
                }
                else if (c == '{' || c == '[') {
                    ++depth;
                }
                else if (c == '}' || c == ']') {
                    --depth;
                }
                else if (c == separator && depth == 0) {
                    parts.push_back(text.substr(start, i - start));
                    start = i + 1;
                }
            }
            parts.push_back(text.substr(start));
            return parts;
        }

        // The value is written as the named tag the client reads with NbtIo.readAnyTag:
        // a type byte followed by the payload, with no name. The writer refuses values it
        // cannot encode (a string over 65535 bytes, a mixed list), and that has to be an
        // error here: a half written component would break the whole packet.
        std::optional<std::vector<std::uint8_t>> component_nbt(const terila::nbt::NbtValue& value) {
            terila::nbt::Writer writer(terila::nbt::StringFormat::Network);
            writer.write_value(value);
            auto payload = writer.finish();
            if (payload.empty()) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes;
            bytes.reserve(payload.size() + 1);
            bytes.push_back(static_cast<std::uint8_t>(value.tag()));
            bytes.insert(bytes.end(), payload.begin(), payload.end());
            return bytes;
        }

        // A chat component: a plain string is the shorthand for {"text": "..."}.
        std::optional<std::vector<std::uint8_t>> text_component_nbt(const terila::nbt::NbtValue& value) {
            if (const auto* text = value.get_if<std::string>(); text != nullptr) {
                terila::nbt::NbtCompound compound;
                compound.insert_or_assign("text", terila::nbt::NbtValue(*text));
                return component_nbt(terila::nbt::NbtValue(std::move(compound)));
            }
            return component_nbt(value);
        }

        bool build_enchantments(const terila::nbt::NbtValue& value, const ComponentIds& ids,
            Components& out, std::string& error) {
            const auto* compound = value.get_if<terila::nbt::NbtCompound>();
            if (compound == nullptr) {
                error = "minecraft:enchantments expects {levels:{...}}";
                return false;
            }
            out.enchantments_present = true;
            if (compound->empty()) {
                return true;
            }
            const auto levels = compound->find("levels");
            if (levels == compound->end()) {
                error = "minecraft:enchantments expects {levels:{...}}";
                return false;
            }
            const auto* entries = levels->second.get_if<terila::nbt::NbtCompound>();
            if (entries == nullptr) {
                error = "minecraft:enchantments levels must be a compound";
                return false;
            }

            for (const auto& [name, level] : *entries) {
                const auto* raw = level.get_if<std::int32_t>();
                if (raw == nullptr) {
                    error = std::format("enchantment level of {} is not an integer", name);
                    return false;
                }
                if (*raw < 1 || *raw > 255) {
                    error = std::format("enchantment level {} is outside 1..255", *raw);
                    return false;
                }
                const std::string id = namespaced(lower(name));
                if (ids.enchantment_registry != nullptr &&
                    ids.enchantment_registry->find(id) == ids.enchantment_registry->end()) {
                    error = "unknown enchantment: " + id;
                    return false;
                }
                out.set_enchantment(id, *raw);
            }
            return true;
        }

        bool apply_component(std::string_view name, std::string_view value_text,
            const ComponentIds& ids, Components& out, std::string& error) {
            const std::string id = namespaced(lower(std::string(trim(name))));
            const std::int32_t type_id = ids.find(id);
            if (type_id < 0) {
                error = "unknown component: " + id;
                return false;
            }

            const auto value = terila::nbt::parse_snbt(trim(value_text));
            if (!value) {
                error = "malformed value for " + id;
                return false;
            }

            if (id == "minecraft:enchantments") {
                return build_enchantments(*value, ids, out, error);
            }
            if (id == "minecraft:custom_name" || id == "minecraft:item_name") {
                auto encoded = text_component_nbt(*value);
                if (!encoded) {
                    error = "cannot encode " + id;
                    return false;
                }
                if (id == "minecraft:custom_name") {
                    out.custom_name_nbt = std::move(*encoded);
                }
                else {
                    out.item_name_nbt = std::move(*encoded);
                }
                return true;
            }
            if (id == "minecraft:damage" || id == "minecraft:max_stack_size") {
                const auto* number = value->get_if<std::int32_t>();
                if (number == nullptr || *number < 0) {
                    error = id + " expects a non negative integer";
                    return false;
                }
                if (id == "minecraft:damage") {
                    out.damage = *number;
                }
                else {
                    out.max_stack_size = *number;
                }
                return true;
            }
            if (id == "minecraft:unbreakable") {
                // Unit: only the presence of the component carries meaning.
                if (value->get_if<terila::nbt::NbtCompound>() == nullptr) {
                    error = "minecraft:unbreakable expects {}";
                    return false;
                }
                out.unbreakable = true;
                return true;
            }

            error = "component " + id + " is not supported yet";
            return false;
        }

    } // namespace

    std::string namespaced(std::string_view name) {
        return name.find(':') == std::string_view::npos
            ? "minecraft:" + std::string(name)
            : std::string(name);
    }

    std::optional<ParsedItem> parse_item(std::string_view text, const ComponentIds& ids,
        std::string& error) {
        ParsedItem item;
        const auto open = text.find('[');
        const std::string_view id_part = trim(open == std::string_view::npos ? text : text.substr(0, open));
        if (id_part.empty()) {
            error = "missing item id";
            return std::nullopt;
        }
        item.id = namespaced(lower(std::string(id_part)));

        if (open == std::string_view::npos) {
            return item;
        }
        const std::string_view components = trim(text.substr(open + 1));
        if (components.empty() || components.back() != ']') {
            error = "component list must end with ]";
            return std::nullopt;
        }
        const std::string_view inner = trim(components.substr(0, components.size() - 1));
        if (inner.empty()) {
            return item;   // diamond_sword[] means the plain item
        }

        for (const auto part : split_top_level(inner, ',')) {
            const auto entry = trim(part);
            if (entry.empty()) {
                continue;
            }
            if (entry.front() == '!') {
                const std::string removed = namespaced(lower(std::string(trim(entry.substr(1)))));
                const std::int32_t type_id = ids.find(removed);
                if (type_id < 0) {
                    error = "unknown component: " + removed;
                    return std::nullopt;
                }
                item.components.removed.push_back(type_id);
                continue;
            }
            const auto equals = entry.find('=');
            if (equals == std::string_view::npos) {
                error = "component needs a value: " + std::string(entry);
                return std::nullopt;
            }
            if (!apply_component(entry.substr(0, equals), entry.substr(equals + 1), ids,
                item.components, error)) {
                return std::nullopt;
            }
        }
        return item;
    }

} // namespace terila::item
