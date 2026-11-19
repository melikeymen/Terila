#include "commands/dispatch.hpp"

#include "commands/command_parse.hpp"
#include "commands/text.hpp"
#include "item/item_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace terila::commands {

    namespace {

        constexpr std::int32_t kMaxStack = 64;

        bool parse_int(std::string_view text, std::int32_t& out) {
            if (text.empty()) {
                return false;
            }
            try {
                std::size_t used = 0;
                const auto parsed = std::stoll(std::string(text), &used);
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

        bool parse_i64(std::string_view text, std::int64_t& out) {
            if (text.empty()) {
                return false;
            }
            try {
                std::size_t used = 0;
                const auto parsed = std::stoll(std::string(text), &used);
                if (used != text.size()) {
                    return false;
                }
                out = parsed;
                return true;
            }
            catch (...) {
                return false;
            }
        }

        // The item argument can hold a quoted name (minecraft:custom_name='"Big Sword"'),
        // so it is read from the raw text instead of a whitespace split: spaces inside
        // quotes or brackets belong to the argument.
        std::string_view read_item_token(std::string_view text, std::string_view& rest) {
            text.remove_prefix(static_cast<std::size_t>(std::distance(text.begin(),
                std::find_if(text.begin(), text.end(),
                    [](char c) { return std::isspace(static_cast<unsigned char>(c)) == 0; }))));

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

        constexpr const char* kGiveUsage =
            "Usage: /give [@a|@s|@p|@r] <item>[<component>=<value>,...] [count]";

        // /give <item>[<component>=<value>,...] [count]
        // /give <targets> <item>[<component>=<value>,...] [count]
        void run_give(const Context& context, std::string_view arguments) {
            if (arguments.empty()) {
                context.reply(kGiveUsage);
                return;
            }

            std::string_view after_item;
            std::string_view item_token = read_item_token(arguments, after_item);

            // A target selector only makes sense in front of the item, so it is the first
            // token or it is not there at all. A plain player name counts as a target too,
            // which is why the first token is only read as an item when nobody is called
            // that.
            std::optional<Selector> selector;
            const auto names_a_player = [&context](std::string_view token) {
                if (is_selector(token)) {
                    return true;
                }
                if (!context.players) {
                    return false;
                }
                for (const auto& player : context.players()) {
                    if (equal_ignore_case(player.name, token)) {
                        return true;
                    }
                }
                return false;
                };
            if (names_a_player(item_token)) {
                selector = parse(item_token);
                if (!selector) {
                    context.reply("Cannot read that target selector");
                    return;
                }
                std::string_view tail;
                item_token = read_item_token(after_item, tail);
                after_item = tail;
                if (item_token.empty()) {
                    context.reply("Usage: /give " + selector->text +
                        " <item>[<component>=<value>,...] [count]");
                    return;
                }
            }
            const std::string_view count_text = trim(after_item);
            if (context.component_ids == nullptr) {
                context.reply("Commands are not available");
                return;
            }

            std::string error;
            auto parsed = terila::item::parse_item(item_token, *context.component_ids, error);
            if (!parsed) {
                context.reply(error.empty() ? std::string("Cannot read that item") : error);
                return;
            }

            std::int32_t item_id = 0;
            if (context.item_ids != nullptr) {
                const auto found = context.item_ids->find(parsed->id);
                if (found != context.item_ids->end()) {
                    item_id = found->second;
                }
            }
            if (item_id == 0) {
                context.reply("Unknown item: " + parsed->id);
                return;
            }

            std::int32_t count = 1;
            if (!count_text.empty()) {
                std::int32_t requested = 0;
                if (!parse_int(count_text, requested)) {
                    context.reply("Count must be a number");
                    return;
                }
                count = std::clamp(requested, 1, kMaxStack);
            }

            terila::item::ComponentsPtr components;
            if (!parsed->components.empty()) {
                components = std::make_shared<terila::item::Components>(
                    std::move(parsed->components));
            }

            std::vector<Target> receivers;
            if (selector) {
                receivers = resolve(*selector, context.self,
                    context.players ? context.players() : std::vector<Target>{ context.self });
            }
            else {
                receivers.push_back(context.self);
            }
            if (receivers.empty()) {
                context.reply("No player matched '" +
                    (selector ? selector->text : context.self.name) + "'");
                return;
            }

            std::size_t delivered = 0;
            for (const auto& target : receivers) {
                if (context.give && context.give(target.entity, item_id, count, components)) {
                    ++delivered;
                }
            }
            if (delivered == 0) {
                context.reply(selector ? "No player could take " + parsed->id
                                      : std::string("Your inventory is full"));
                return;
            }

            std::string message = selector
                ? std::format("Gave {} x{} to {} player{}", parsed->id, count, delivered,
                    delivered == 1 ? "" : "s")
                : std::format("Gave {} x{}", parsed->id, count);
            if (components != nullptr) {
                const auto detail = describe(*components);
                if (!detail.empty()) {
                    message += " with " + detail;
                }
            }
            context.reply(message);
        }

        constexpr const char* kTimeUsage =
            "Usage: /time set <day|noon|sunset|night|midnight|sunrise|<ticks>>, "
            "/time add <ticks>, /time query <daytime|gametime>";

        // /time set day|noon|sunset|night|midnight|sunrise|<ticks>
        // /time add <ticks>
        // /time query daytime|gametime
        void run_time(const Context& context, std::string_view arguments) {
            if (context.time == nullptr) {
                context.reply("This world has no clock");
                return;
            }
            const auto& time = *context.time;

            const auto split = arguments.find(' ');
            const std::string action = lower(trim(arguments.substr(0, split)));
            const std::string value = split == std::string_view::npos
                ? std::string{}
                : lower(trim(arguments.substr(split)));

            if (action == "set" || action == "add") {
                std::int64_t ticks = 0;
                if (value == "day") ticks = 1000;
                else if (value == "noon") ticks = 6000;
                else if (value == "sunset") ticks = 12000;
                else if (value == "night") ticks = 13000;
                else if (value == "midnight") ticks = 18000;
                else if (value == "sunrise") ticks = 23000;
                else if (!parse_i64(value, ticks)) {
                    context.reply(kTimeUsage);
                    return;
                }

                if (action == "set") {
                    time.set(ticks);
                }
                else {
                    time.add(ticks);
                }
                time.sync();

                context.reply(std::format("Set the time to {} ({})", time.time_of_day(),
                    time.night() ? "night" : "day"));
                return;
            }

            if (action == "query") {
                if (value == "gametime" || value.empty()) {
                    context.reply(std::format("The world has run {} ticks", time.game_time()));
                }
                else {
                    context.reply(std::format("The time is {} ({})", time.time_of_day(),
                        time.night() ? "night" : "day"));
                }
                return;
            }

            context.reply(kTimeUsage);
        }

        constexpr const char* kExecuteUsage = "Usage: /execute [as <targets>] run <command>";

        // /execute [as <targets>] run <command>
        //
        // The rest of the game's grammar (at, positioned, if, store) is not implemented:
        // every command this server runs acts on a player, not on a position, so the only
        // context that changes anything is whose player it is.
        void run_execute(const Context& context, std::string_view arguments) {
            const auto parsed = parse_execute(arguments);
            if (!parsed) {
                context.reply(kExecuteUsage);
                return;
            }
            if (parsed->selector.empty()) {
                if (context.run) {
                    context.run(parsed->command);
                }
                return;
            }

            const auto selector = parse(parsed->selector);
            if (!selector) {
                context.reply("Cannot read that target selector");
                return;
            }
            const auto targets = resolve(*selector, context.self,
                context.players ? context.players() : std::vector<Target>{ context.self });
            if (targets.empty()) {
                context.reply("No player matched '" + parsed->selector + "'");
                return;
            }

            for (const auto& target : targets) {
                if (context.run_as && !context.run_as(target.entity, parsed->command)) {
                    context.reply("No player to run that command as");
                }
            }
        }

    } // namespace

    bool dispatch(const Context& context, std::string_view line) {
        std::string_view rest = trim(line);
        if (rest.empty()) {
            return false;
        }

        const auto name_end = rest.find_first_of(" \t");
        const std::string name = lower(rest.substr(0, name_end));
        const std::string_view arguments = name_end == std::string_view::npos
            ? std::string_view{}
            : trim(rest.substr(name_end));

        if (name == "give") {
            run_give(context, arguments);
            return true;
        }
        if (name == "execute") {
            run_execute(context, arguments);
            return true;
        }
        if (name == "time") {
            run_time(context, arguments);
            return true;
        }
        return false;
    }

} // namespace terila::commands
