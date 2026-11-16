#pragma once

// How long a block resists being dug at, and what the held tool changes about it.
//
// The game describes a block's resistance as "hardness", in seconds of work: a block that
// yields falls in hardness × 1.5 seconds, one that needs a tool the player is not holding
// takes five times as long. A tool that is good at the block divides that time by its own
// speed (a diamond pickaxe is 8). Negative hardness is bedrock and friends, which never yield.
//
// The rules below are the item's own: `minecraft:tool` names the blocks an item helps with,
// either one block or a tag, and breaking only has to look the block up in that list.

#include <cmath>
#include <string>
#include <vector>
#include <unordered_set>

namespace terila::world {

    // One entry of an item's `minecraft:tool` rules. `blocks` is a block id, or a tag id when
    // it starts with '#'.
    struct ToolRule {
        std::string blocks;
        bool correct_for_drops = false;
        float speed = 1.0f;
    };

    // What the held item does to one particular block.
    struct ToolAgainstBlock {
        float speed = 1.0f;
        bool can_harvest = true;
    };

    // Seconds of work per point of hardness: 1.5 s when the block yields, 5 s when it wants a
    // tool that is not being used.
    inline constexpr double kYieldSecondsPerHardness = 1.5;
    inline constexpr double kRefuseSecondsPerHardness = 5.0;
    inline constexpr double kTicksPerSecond = 20.0;

    // Applies the item's rules in their own order: the first rule the block answers to wins.
    // `tags` may be null when the block is in no tags at all.
    inline ToolAgainstBlock tool_against(const std::vector<ToolRule>& rules, const std::string& block,
        const std::unordered_set<std::string>* tags, bool requires_correct_tool) noexcept {
        ToolAgainstBlock out;
        for (const auto& rule : rules) {
            const bool tag_rule = rule.blocks.size() > 1 && rule.blocks.front() == '#';
            const bool matches = tag_rule
                ? (tags != nullptr && tags->count(rule.blocks.substr(1)) != 0)
                : rule.blocks == block;
            if (!matches) {
                continue;
            }

            out.speed = rule.speed;
            out.can_harvest = !requires_correct_tool || rule.correct_for_drops;
            return out;
        }

        // Nothing in the item helps here: it digs like a bare hand, and only a block that asks
        // for no tool at all still gives anything up.
        out.can_harvest = !requires_correct_tool;
        return out;
    }

    // Ticks until the block gives way; -1 when it never does.
    inline int break_ticks(float hardness, bool can_harvest, float speed) noexcept {
        if (!(hardness >= 0.0f)) {   // also catches NaN
            return -1;
        }
        const double per_hardness = can_harvest ? kYieldSecondsPerHardness : kRefuseSecondsPerHardness;
        const double tool_speed = (speed >= 1.0f) ? static_cast<double>(speed) : 1.0;
        const double ticks = static_cast<double>(hardness) * per_hardness * kTicksPerSecond / tool_speed;
        return static_cast<int>(std::ceil(ticks));
    }

} // namespace terila::world
