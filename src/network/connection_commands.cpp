#include "network/connection.hpp"

#include "item/item_parser.hpp"
#include "protocol/chat.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace terila::network {

    namespace {

        std::string lower(std::string value) {
            for (char& c : value) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return value;
        }

        bool parse_int(const std::string& text, std::int32_t& out) {
            try {
                std::size_t used = 0;
                const auto parsed = std::stoll(text, &used);
                if (used != text.size()) {
                    return false;
                }
                out = static_cast<std::int32_t>(parsed);
                return true;
            }
            catch (...) {
                return false;
            }
        }

        std::string_view trim(std::string_view text) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
                text.remove_prefix(1);
            }
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
                text.remove_suffix(1);
            }
            return text;
        }

        // The item argument can hold a quoted name (minecraft:custom_name='"Big Sword"'),
        // so it is read from the raw text instead of a whitespace split: spaces inside
        // quotes or brackets belong to the argument.
        std::string_view read_item_token(std::string_view text, std::string_view& rest) {
            int depth = 0;
            char quote = 0;
            std::size_t i = 0;
            for (; i < text.size(); ++i) {
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
                else if (c == '[' || c == '{') {
                    ++depth;
                }
                else if (c == ']' || c == '}') {
                    --depth;
                }
                else if (depth == 0 && std::isspace(static_cast<unsigned char>(c)) != 0) {
                    break;
                }
            }
            rest = text.substr(i);
            return text.substr(0, i);
        }

        // A short recap of what the patch carries, for the confirmation message.
        std::string describe(const terila::item::Components& components) {
            std::vector<std::string> parts;
            for (const auto& enchantment : components.enchantments) {
                parts.push_back(std::format("{} {}", enchantment.id, enchantment.level));
            }
            if (!components.custom_name_nbt.empty() || !components.custom_name.empty()) {
                parts.push_back("custom_name");
            }
            if (!components.item_name_nbt.empty() || !components.item_name.empty()) {
                parts.push_back("item_name");
            }
            if (components.damage.has_value()) {
                parts.push_back(std::format("damage {}", *components.damage));
            }
            if (components.max_stack_size.has_value()) {
                parts.push_back(std::format("max_stack_size {}", *components.max_stack_size));
            }
            if (components.unbreakable) {
                parts.push_back("unbreakable");
            }
            if (!components.removed.empty()) {
                parts.push_back(std::format("{} removed", components.removed.size()));
            }
            std::string text;
            for (const auto& part : parts) {
                if (!text.empty()) {
                    text += ", ";
                }
                text += part;
            }
            return text;
        }

    } // namespace

    void Connection::send_system_message(std::string_view text) {
        std::vector<std::uint8_t> out;
        if (terila::protocol::write_system_chat(out, text)) {
            send(out);
        }
    }

    std::int32_t Connection::resolve_item(std::string_view name) const {
        const std::string id = terila::item::namespaced(lower(std::string(name)));
        const auto it = data_.item_ids.find(id);
        return it == data_.item_ids.end() ? 0 : it->second;
    }

    bool Connection::give_item(std::int32_t item_id, std::int32_t count,
        const terila::item::ComponentsPtr& components) {
        return add_to_inventory(item_id, count, components);
    }

    // A selector picks who receives the item; an empty one means "me". The hub knows the
    // other players and each connection knows how to fill its own inventory, so the two
    // meet on the giver callback the hub keeps for every member.
    std::size_t Connection::give_to_targets(std::string_view selector, std::int32_t item_id,
        std::int32_t count, const terila::item::ComponentsPtr& components) {
        if (selector.empty() || selector == "@s") {
            return give_item(item_id, count, components) ? 1 : 0;
        }
        if (hub_ == nullptr) {
            return give_item(item_id, count, components) ? 1 : 0;
        }
        if (selector == "@a" || selector == "@e") {
            return hub_->give_to_all(item_id, count, components);
        }
        if (selector == "@p") {
            const auto nearest = hub_->nearest_to(player_x_, player_z_);
            return hub_->give_to(nearest, item_id, count, components) ? 1 : 0;
        }
        if (selector == "@r") {
            const auto picked = hub_->random_member();
            return hub_->give_to(picked, item_id, count, components) ? 1 : 0;
        }
        LOG_DEBUG(Network, "unknown selector '{}' in /give", selector);
        return 0;
    }

    // Commands:
    //   /give <item>[<component>=<value>,...] [count]
    void Connection::handle_command(const std::string& command) {
        std::string_view rest = trim(command);
        if (rest.empty()) {
            return;
        }

        const auto name_end = rest.find_first_of(" \t");
        const std::string name = lower(std::string(rest.substr(0, name_end)));
        const std::string_view arguments = name_end == std::string_view::npos
            ? std::string_view{}
            : trim(rest.substr(name_end));

        if (name == "give") {
            if (arguments.empty()) {
                send_system_message(
                    "Usage: /give [@a|@s|@p|@r] <item>[<component>=<value>,...] [count]");
                return;
            }

            std::string_view after_item;
            std::string_view item_token = read_item_token(arguments, after_item);

            // A target selector only makes sense in front of the item, so it is the first
            // token or it is not there at all.
            std::string selector;
            if (!item_token.empty() && item_token.front() == '@') {
                selector = lower(std::string(item_token));
                std::string_view tail;
                item_token = read_item_token(after_item, tail);
                after_item = tail;
            }
            const std::string_view count_text = trim(after_item);

            std::string error;
            auto parsed = terila::item::parse_item(item_token, component_ids_, error);
            if (!parsed) {
                send_system_message(error.empty() ? std::string("Cannot read that item") : error);
                return;
            }

            const std::int32_t item_id = resolve_item(parsed->id);
            if (item_id == 0) {
                send_system_message("Unknown item: " + parsed->id);
                return;
            }

            std::int32_t count = 1;
            if (!count_text.empty()) {
                std::int32_t requested = 0;
                if (!parse_int(std::string(count_text), requested)) {
                    send_system_message("Count must be a number");
                    return;
                }
                count = std::clamp(requested, 1, kMaxStackSize);
            }

            terila::item::ComponentsPtr components;
            if (!parsed->components.empty()) {
                components = std::make_shared<terila::item::Components>(
                    std::move(parsed->components));
            }

            const auto delivered = give_to_targets(selector, item_id, count, components);
            if (delivered == 0) {
                send_system_message(selector.empty()
                    ? std::string("Your inventory is full")
                    : "No player could take " + parsed->id);
                return;
            }

            std::string message = selector.empty()
                ? std::format("Gave {} x{}", parsed->id, count)
                : std::format("Gave {} x{} to {} player{}", parsed->id, count, delivered,
                    delivered == 1 ? "" : "s");
            if (components != nullptr) {
                const auto detail = describe(*components);
                if (!detail.empty()) {
                    message += " with " + detail;
                }
            }
            send_system_message(message);
            return;
        }

        send_system_message(
            "Commands: /give [@a|@s|@p|@r] <item>[<component>=<value>,...] [count]");
    }

} // namespace terila::network
