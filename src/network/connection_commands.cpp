#include "network/connection.hpp"

#include "protocol/chat.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>
#include <string>
#include <vector>

namespace terila::network {

    namespace {

        std::vector<std::string> split_words(const std::string& text) {
            std::vector<std::string> words;
            std::istringstream stream(text);
            std::string word;
            while (stream >> word) {
                words.push_back(std::move(word));
            }
            return words;
        }

        std::string lower(std::string value) {
            for (char& c : value) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return value;
        }

        // "diamond_sword" and "minecraft:diamond_sword" both mean the vanilla item.
        std::string namespaced(const std::string& name) {
            return name.find(':') == std::string::npos ? "minecraft:" + name : name;
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

    } // namespace

    void Connection::send_system_message(std::string_view text) {
        std::vector<std::uint8_t> out;
        if (terila::protocol::write_system_chat(out, text)) {
            send(out);
        }
    }

    std::int32_t Connection::resolve_item(std::string_view name) const {
        const auto it = data_.item_ids.find(namespaced(std::string(name)));
        return it == data_.item_ids.end() ? 0 : it->second;
    }

    bool Connection::give_item(std::int32_t item_id, std::int32_t count,
        const terila::item::ComponentsPtr& components) {
        return add_to_inventory(item_id, count, components);
    }

    // Enchanting works on the selected hotbar slot so the result is visible immediately.
    void Connection::enchant_held_item(std::string_view enchantment, std::int32_t level) {
        const std::int32_t index = kMainInventorySlots + carried_slot_;
        auto& slot = inventory_[static_cast<std::size_t>(index)];
        if (slot.item_id <= 0 || slot.count <= 0) {
            send_system_message("Hold an item in your hand first");
            return;
        }

        auto updated = slot.components != nullptr
            ? std::make_shared<terila::item::Components>(*slot.components)
            : std::make_shared<terila::item::Components>();
        const std::string id = namespaced(lower(std::string(enchantment)));
        updated->set_enchantment(id, level);
        slot.components = updated;

        send_inventory_slot(kFirstInventorySlot + index);
        refresh_held_item();
        send_system_message(std::format("Applied {} {} to the held item", id, level));
    }

    // Commands:
    //   /give <item> [count] [enchantment] [level]
    //   /enchant <enchantment> <level>
    void Connection::handle_command(const std::string& command) {
        const auto words = split_words(command);
        if (words.empty()) {
            return;
        }

        const std::string name = lower(words[0]);

        if (name == "give") {
            if (words.size() < 2) {
                send_system_message("Usage: /give <item> [count] [enchantment] [level]");
                return;
            }

            const std::int32_t item_id = resolve_item(words[1]);
            if (item_id == 0) {
                send_system_message("Unknown item: " + words[1]);
                return;
            }

            std::size_t next = 2;
            std::int32_t count = 1;
            std::int32_t parsed = 0;
            if (next < words.size() && parse_int(words[next], parsed)) {
                count = std::clamp(parsed, 1, kMaxStackSize);
                ++next;
            }

            terila::item::ComponentsPtr components;
            if (next + 1 < words.size()) {
                std::int32_t level = 0;
                if (!parse_int(words[next + 1], level)) {
                    send_system_message("Enchantment level must be a number");
                    return;
                }
                auto built = std::make_shared<terila::item::Components>();
                built->set_enchantment(namespaced(lower(words[next])), level);
                components = built;
            }

            if (!give_item(item_id, count, components)) {
                send_system_message("Your inventory is full");
                return;
            }

            std::string message = std::format("Gave {} x{}", namespaced(words[1]), count);
            if (components) {
                message += std::format(" with {} {}", components->enchantments.front().id,
                    components->enchantments.front().level);
            }
            send_system_message(message);
            return;
        }

        if (name == "enchant") {
            if (words.size() < 3) {
                send_system_message("Usage: /enchant <enchantment> <level>");
                return;
            }
            std::int32_t level = 0;
            if (!parse_int(words[2], level)) {
                send_system_message("Enchantment level must be a number");
                return;
            }
            enchant_held_item(words[1], level);
            return;
        }

        send_system_message("Commands: /give <item> [count] [enchantment] [level], "
            "/enchant <enchantment> <level>");
    }

} // namespace terila::network
