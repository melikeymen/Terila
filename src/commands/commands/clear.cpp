#include "commands/registry.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        Result run_clear(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            std::int32_t item_id = 0;
            std::string described = "items";
            if (const auto* stack = exec.args.get<ItemStackValue>("item")) {
                const auto found = exec.registry.data().item_ids.find(stack->id);
                if (found == exec.registry.data().item_ids.end()) {
                    origin.say("Unknown item: " + stack->id);
                    return Result{ 0, false };
                }
                item_id = found->second;
                described = stack->id;
            }

            int max_count = 0;
            if (const auto* limit = exec.args.get<std::int64_t>("max_count")) {
                max_count = static_cast<int>(std::min<std::int64_t>(*limit, 2147483647));
            }

            std::vector<Target> players;
            const auto* selector = exec.args.get<Selector>("targets");
            if (selector != nullptr) {
                std::string error;
                players = targets_of(*selector, origin, error);
                if (players.empty()) {
                    origin.say(error.empty() ? "No player was found" : error);
                    return Result{ 0, false };
                }
            }
            else {
                if (origin.console) {
                    origin.say("A player is required to clear an inventory");
                    return Result{ 0, false };
                }
                players.push_back(Target{ origin.entity, origin.name, origin.x, origin.y,
                    origin.z });
            }

            std::int32_t removed = 0;
            std::int32_t emptied = 0;
            for (const auto& player : players) {
                const std::int32_t count = origin.game->clear_items(player.entity, item_id,
                    max_count);
                removed += count;
                if (count > 0) {
                    ++emptied;
                }
            }

            if (emptied == 0) {
                origin.say(item_id == 0 ? "No items were found"
                                        : std::format("No {} were found", described));
                return Result{ 0, false };
            }
            if (emptied == 1 && players.size() == 1) {
                origin.say(std::format("Removed {} item{}", removed,
                    removed == 1 ? "" : "s"));
            }
            else {
                origin.say(std::format("Removed {} item{} from {} player{}", removed,
                    removed == 1 ? "" : "s", emptied, emptied == 1 ? "" : "s"));
            }
            return Result{ removed, true };
        }

    } // namespace

    void install_clear(Registry& registry) {
        Node& clear = registry.root().literal("clear", Capability::Helper);
        clear.executes(run_clear);
        Node& targets = clear.argument("targets", Parser::players());
        targets.executes(run_clear);
        Node& item = targets.argument("item", registry.item_parser());
        item.executes(run_clear);
        item.argument("max_count", Parser::integer(0)).executes(run_clear);
    }

} // namespace terila::commands
