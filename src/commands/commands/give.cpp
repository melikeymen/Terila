#include "commands/registry.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        constexpr std::int32_t kMaxStack = 64;

        std::string describe(const item::Components& components) {
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

        Result run_give(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            const auto* stack = exec.args.get<ItemStackValue>("item");
            if (stack == nullptr) {
                return Result{ 0, false };
            }

            std::int32_t count = 1;
            if (const auto* requested = exec.args.get<std::int64_t>("count")) {
                count = std::clamp(static_cast<std::int32_t>(*requested), 1, kMaxStack);
            }

            const auto found = exec.registry.data().item_ids.find(stack->id);
            if (found == exec.registry.data().item_ids.end()) {
                origin.say("Unknown item: " + stack->id);
                return Result{ 0, false };
            }
            const std::int32_t item_id = found->second;

            std::vector<Target> receivers;
            const auto* selector = exec.args.get<Selector>("targets");
            if (selector != nullptr) {
                std::string error;
                receivers = targets_of(*selector, origin, error);
                if (receivers.empty()) {
                    origin.say(error.empty() ? "No player matched" : error);
                    return Result{ 0, false };
                }
            }
            else {
                if (origin.console) {
                    origin.say("A player is required to receive the item");
                    return Result{ 0, false };
                }
                receivers.push_back(Target{ origin.entity, origin.name, origin.x, origin.y,
                    origin.z });
            }

            std::size_t delivered = 0;
            for (const auto& target : receivers) {
                if (origin.game->give(target.entity, item_id, count, stack->components)) {
                    ++delivered;
                }
            }
            if (delivered == 0) {
                origin.say(selector != nullptr ? "No player could take " + stack->id
                                               : std::string("Your inventory is full"));
                return Result{ 0, false };
            }

            std::string message = selector != nullptr
                ? std::format("Gave {} x{} to {} player{}", stack->id, count, delivered,
                    delivered == 1 ? "" : "s")
                : std::format("Gave {} x{}", stack->id, count);
            if (stack->components != nullptr) {
                const auto detail = describe(*stack->components);
                if (!detail.empty()) {
                    message += " with " + detail;
                }
            }
            origin.say(message);
            return Result{ static_cast<std::int32_t>(delivered), true };
        }

    } // namespace

    void install_give(Registry& registry) {
        Node& give = registry.root().literal("give");

        Node& direct = give.argument("item", registry.item_parser());
        direct.executes(run_give);
        direct.argument("count", Parser::integer(1)).executes(run_give);

        Node& selected = give.argument("targets", Parser::players());
        Node& selected_item = selected.argument("item", registry.item_parser());
        selected_item.executes(run_give);
        selected_item.argument("count", Parser::integer(1)).executes(run_give);
    }

} // namespace terila::commands
