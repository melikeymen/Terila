#pragma once

#include "commands/cursor.hpp"
#include "commands/arguments/text.hpp"
#include "commands/arguments/value.hpp"
#include "data/registry_data.hpp"
#include "item/item_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace terila::commands {

    enum class ParserKind {
        Boolean,
        Integer,
        Real,
        Text,          // one word or a quoted phrase
        GreedyText,    // the rest of the line
        PlayerNames,   // selector or list of names, any number
        OnePlayer,     // selector or name, exactly one
        Gamemode,
        Position,
        Rotation,
        ItemStack,
        ResourceLocation,
        BlockState
    };

    // One argument of a command. The wire name is the registry id the client knows the
    // parser by; the bounds and flags below are the property bytes that go with it.
    struct Parser {
        ParserKind kind = ParserKind::Text;
        std::string wire = "brigadier:string";
        std::int64_t min = 0;
        std::int64_t max = 0;
        // A real argument carries its bounds as doubles on the wire; the integers above
        // stay whole so the parser keeps one comparison path.
        double real_min = 0.0;
        double real_max = 0.0;
        bool has_min = false;
        bool has_max = false;
        bool single = false;
        bool players_only = true;
        int text_mode = 0;   // 0 one word, 1 phrase, 2 greedy

        // The item ids and component registry /give needs, filled in by the registry.
        const std::unordered_map<std::string, std::int32_t>* item_ids = nullptr;
        const item::ComponentIds* components = nullptr;
        // The block tables /setblock and /fill resolve their state against.
        const std::unordered_map<std::string, std::vector<data::BlockStateInfo>>* block_states =
            nullptr;
        const std::unordered_map<std::string, std::int32_t>* block_state_ids = nullptr;

        static Parser integer(std::int64_t min, bool has_max = false, std::int64_t max = 0) {
            Parser parser;
            parser.kind = ParserKind::Integer;
            parser.wire = "brigadier:integer";
            parser.min = min;
            parser.has_min = true;
            parser.max = max;
            parser.has_max = has_max;
            return parser;
        }

        static Parser real(double min, bool has_max = false, double max = 0.0) {
            Parser parser;
            parser.kind = ParserKind::Real;
            parser.wire = "brigadier:double";
            parser.min = static_cast<std::int64_t>(min);
            parser.has_min = true;
            parser.max = static_cast<std::int64_t>(max);
            parser.has_max = has_max;
            parser.real_min = min;
            parser.real_max = max;
            return parser;
        }

        static Parser boolean() {
            Parser parser;
            parser.kind = ParserKind::Boolean;
            parser.wire = "brigadier:bool";
            return parser;
        }

        static Parser text(int mode = 0) {
            Parser parser;
            parser.kind = ParserKind::Text;
            parser.wire = "brigadier:string";
            parser.text_mode = mode;
            return parser;
        }

        static Parser greedy() {
            Parser parser;
            parser.kind = ParserKind::GreedyText;
            parser.wire = "brigadier:string";
            parser.text_mode = 2;
            return parser;
        }

        static Parser players() {
            Parser parser;
            parser.kind = ParserKind::PlayerNames;
            parser.wire = "minecraft:entity";
            parser.players_only = true;
            return parser;
        }

        static Parser one_player() {
            Parser parser = players();
            parser.kind = ParserKind::OnePlayer;
            parser.single = true;
            return parser;
        }

        // Any entity, not only a player: @e reaches the mobs behind it.
        static Parser entities() {
            Parser parser = players();
            parser.players_only = false;
            return parser;
        }

        // A chat line: the rest of the command, sent to the client as a component.
        static Parser message() {
            Parser parser;
            parser.kind = ParserKind::GreedyText;
            parser.wire = "minecraft:message";
            parser.text_mode = 2;
            return parser;
        }

        static Parser gamemode() {
            Parser parser;
            parser.kind = ParserKind::Gamemode;
            parser.wire = "minecraft:gamemode";
            return parser;
        }

        static Parser position() {
            Parser parser;
            parser.kind = ParserKind::Position;
            parser.wire = "minecraft:vec3";
            return parser;
        }

        static Parser rotation() {
            Parser parser;
            parser.kind = ParserKind::Rotation;
            parser.wire = "minecraft:rotation";
            return parser;
        }

        static Parser item_stack() {
            Parser parser;
            parser.kind = ParserKind::ItemStack;
            parser.wire = "minecraft:item_stack";
            return parser;
        }

        static Parser resource_location() {
            Parser parser;
            parser.kind = ParserKind::ResourceLocation;
            parser.wire = "minecraft:resource_location";
            return parser;
        }

        static Parser block_state() {
            Parser parser;
            parser.kind = ParserKind::BlockState;
            parser.wire = "minecraft:block_state";
            return parser;
        }

        std::optional<Value> read(Cursor& cursor) const {
            switch (kind) {
            case ParserKind::Boolean: {
                const auto value = cursor.boolean();
                if (!value) {
                    return std::nullopt;
                }
                return Value{ *value };
            }
            case ParserKind::Integer: {
                const auto value = cursor.integer();
                if (!value) {
                    return std::nullopt;
                }
                if ((has_min && *value < min) || (has_max && *value > max)) {
                    cursor.fail(Problem::Invalid, has_max
                        ? std::to_string(min) + " to " + std::to_string(max) + " is allowed"
                        : "must be at least " + std::to_string(min));
                    return std::nullopt;
                }
                return Value{ *value };
            }
            case ParserKind::Real: {
                const auto value = cursor.real();
                if (!value) {
                    return std::nullopt;
                }
                if ((has_min && *value < static_cast<double>(min)) ||
                    (has_max && *value > static_cast<double>(max))) {
                    cursor.fail(Problem::Invalid, "the number is out of range");
                    return std::nullopt;
                }
                return Value{ *value };
            }
            case ParserKind::Text: {
                const auto value = text_mode == 1 ? cursor.phrase() : cursor.word();
                if (!value) {
                    return std::nullopt;
                }
                return Value{ *value };
            }
            case ParserKind::GreedyText: {
                const auto value = cursor.greedy();
                if (value.empty()) {
                    return std::nullopt;
                }
                return Value{ std::string(value) };
            }
            case ParserKind::PlayerNames:
            case ParserKind::OnePlayer: {
                const auto value = cursor.phrase();
                if (!value) {
                    return std::nullopt;
                }
                const auto selector = commands::parse(*value);
                if (!selector) {
                    cursor.fail(Problem::Invalid, "cannot read the target selector '" + *value + "'");
                    return std::nullopt;
                }
                if (single && !selector->single) {
                    // A list in a one-player slot is not an error the game reports at parse
                    // time; it fails once the list turns out to hold more than one player.
                }
                return Value{ *selector };
            }
            case ParserKind::Gamemode: {
                const auto word = cursor.word();
                if (!word) {
                    return std::nullopt;
                }
                std::string name = lower(*word);
                int mode = -1;
                if (name == "survival" || name == "s") mode = 0;
                else if (name == "creative" || name == "c") mode = 1;
                else if (name == "adventure" || name == "a") mode = 2;
                else if (name == "spectator" || name == "sp") mode = 3;
                if (mode < 0) {
                    cursor.fail(Problem::Invalid, "'" + *word +
                        "' is not a game mode (survival, creative, adventure, spectator)");
                    return std::nullopt;
                }
                return Value{ static_cast<std::int64_t>(mode) };
            }
            case ParserKind::Position: {
                const auto value = read_position(cursor);
                if (!value) {
                    return std::nullopt;
                }
                return Value{ *value };
            }
            case ParserKind::Rotation: {
                const auto value = read_rotation(cursor);
                if (!value) {
                    return std::nullopt;
                }
                return Value{ *value };
            }
            case ParserKind::ItemStack: {
                const auto token = read_item_token(cursor);
                if (!token) {
                    return std::nullopt;
                }
                if (components == nullptr) {
                    cursor.fail(Problem::Failed, "the item registry is not loaded");
                    return std::nullopt;
                }
                std::string error;
                auto parsed = item::parse_item(*token, *components, error);
                if (!parsed) {
                    cursor.fail(Problem::Invalid,
                        error.empty() ? "cannot read that item" : error);
                    return std::nullopt;
                }
                if (item_ids != nullptr) {
                    const auto found = item_ids->find(parsed->id);
                    if (found == item_ids->end()) {
                        cursor.fail(Problem::Invalid, "unknown item '" + parsed->id + "'");
                        return std::nullopt;
                    }
                }
                ItemStackValue stack;
                stack.id = std::move(parsed->id);
                if (!parsed->components.empty()) {
                    stack.components =
                        std::make_shared<const item::Components>(std::move(parsed->components));
                }
                return Value{ std::move(stack) };
            }
            case ParserKind::BlockState: {
                const auto value = read_block_state(cursor);
                if (!value) {
                    return std::nullopt;
                }
                return Value{ *value };
            }
            case ParserKind::ResourceLocation: {
                const auto word = cursor.word();
                if (!word) {
                    return std::nullopt;
                }
                std::string name = lower(*word);
                if (name.find(':') == std::string::npos) {
                    name = "minecraft:" + name;
                }
                if (name.empty() || name.find(' ') != std::string::npos) {
                    cursor.fail(Problem::Invalid, "not a resource location");
                    return std::nullopt;
                }
                return Value{ name };
            }
            }
            return std::nullopt;
        }

    private:
        // One coordinate: a number, `~` with an optional offset, or `^`.
        static bool read_axis(Cursor& cursor, double& out, bool& relative, bool& local) {
            cursor.skip_space();
            const std::string_view text = cursor.text();
            const std::size_t start = cursor.position();
            if (start >= text.size()) {
                cursor.fail(Problem::Incomplete, "expected a coordinate");
                return false;
            }

            relative = false;
            local = false;
            std::size_t at = start;
            if (text[at] == '~') {
                relative = true;
                ++at;
            }
            else if (text[at] == '^') {
                local = true;
                ++at;
            }

            std::size_t end = at;
            while (end < text.size() &&
                std::isspace(static_cast<unsigned char>(text[end])) == 0) {
                ++end;
            }
            if (end == at) {
                out = 0.0;
            }
            else {
                double value = 0.0;
                const auto result =
                    std::from_chars(text.data() + at, text.data() + end, value);
                if (result.ec != std::errc{} || result.ptr != text.data() + end) {
                    cursor.seek(start);
                    cursor.fail(Problem::Invalid, "expected a coordinate");
                    return false;
                }
                out = value;
            }
            cursor.seek(end);
            return true;
        }

        static std::optional<Position> read_position(Cursor& cursor) {
            Position position;
            bool local_x = false;
            bool local_y = false;
            bool local_z = false;
            if (!read_axis(cursor, position.x, position.relative_x, local_x) ||
                !read_axis(cursor, position.y, position.relative_y, local_y) ||
                !read_axis(cursor, position.z, position.relative_z, local_z)) {
                return std::nullopt;
            }
            const int locals = (local_x ? 1 : 0) + (local_y ? 1 : 0) + (local_z ? 1 : 0);
            if (locals != 0 && locals != 3) {
                cursor.fail(Problem::Invalid, "local coordinates need all three axes");
                return std::nullopt;
            }
            if (locals == 3) {
                position.mode = Position::Mode::Local;
            }
            else if (position.relative_x || position.relative_y || position.relative_z) {
                position.mode = Position::Mode::Relative;
            }
            return position;
        }

        static std::optional<RotationValue> read_rotation(Cursor& cursor) {
            RotationValue rotation;
            double yaw = 0.0;
            double pitch = 0.0;
            if (!read_axis(cursor, yaw, rotation.yaw_relative, rotation.pitch_relative) ||
                !read_axis(cursor, pitch, rotation.pitch_relative, rotation.yaw_relative)) {
                return std::nullopt;
            }
            rotation.yaw = static_cast<float>(yaw);
            rotation.pitch = static_cast<float>(pitch);
            return rotation;
        }

        // `<id>` or `<id>[<property>=<value>,...]`, matched against the variant table of
        // the block so the world gets a state id it can store.
        std::optional<BlockStateValue> read_block_state(Cursor& cursor) const {
            const auto token = read_item_token(cursor);
            if (!token) {
                return std::nullopt;
            }
            if (block_states == nullptr || block_state_ids == nullptr) {
                cursor.fail(Problem::Failed, "the block registry is not loaded");
                return std::nullopt;
            }

            std::string name = *token;
            std::vector<std::pair<std::string, std::string>> wanted;
            const auto open = name.find('[');
            if (open != std::string::npos) {
                if (name.size() < open + 2 || name.back() != ']') {
                    cursor.fail(Problem::Invalid, "expected ']' after the block properties");
                    return std::nullopt;
                }
                std::string_view list(name.data() + open + 1, name.size() - open - 2);
                while (!list.empty()) {
                    const auto comma = list.find(',');
                    const std::string_view pair = trim(list.substr(0, comma));
                    if (!pair.empty()) {
                        const auto eq = pair.find('=');
                        if (eq == std::string_view::npos) {
                            cursor.fail(Problem::Invalid, "expected name=value in the block state");
                            return std::nullopt;
                        }
                        wanted.emplace_back(lower(trim(pair.substr(0, eq))),
                            lower(trim(pair.substr(eq + 1))));
                    }
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    list.remove_prefix(comma + 1);
                }
                name.resize(open);
            }

            std::string id = lower(name);
            if (id.find(':') == std::string::npos) {
                id = "minecraft:" + id;
            }

            const auto variants = block_states->find(id);
            if (variants == block_states->end()) {
                cursor.fail(Problem::Invalid, "unknown block '" + id + "'");
                return std::nullopt;
            }

            std::int32_t chosen = -1;
            if (wanted.empty()) {
                const auto single = block_state_ids->find(id);
                if (single != block_state_ids->end()) {
                    chosen = single->second;
                }
            }
            else {
                // The first variant that carries every property the line named; properties
                // the line left out keep whatever that variant defaults them to.
                for (const auto& variant : variants->second) {
                    bool matches = true;
                    for (const auto& [key, value] : wanted) {
                        const auto it = variant.properties.find(key);
                        if (it == variant.properties.end() || it->second != value) {
                            matches = false;
                            break;
                        }
                    }
                    if (matches) {
                        chosen = variant.id;
                        break;
                    }
                }
            }

            if (chosen < 0) {
                cursor.fail(Problem::Invalid, "no state of '" + id + "' has those properties");
                return std::nullopt;
            }
            return BlockStateValue{ chosen, id };
        }

        // The item argument may carry quoted text and component brackets, so it is read
        // from the raw line instead of ending at the first space.
        static std::optional<std::string> read_item_token(Cursor& cursor) {
            cursor.skip_space();
            std::string_view text = cursor.text().substr(cursor.position());
            if (text.empty()) {
                cursor.fail(Problem::Incomplete, "expected an item");
                return std::nullopt;
            }

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
                    if (depth < 0) {
                        cursor.fail(Problem::Invalid, "unbalanced brackets in the item");
                        return std::nullopt;
                    }
                }
                else if (depth == 0 && std::isspace(static_cast<unsigned char>(c)) != 0) {
                    break;
                }
            }
            if (depth != 0 || quote != 0) {
                cursor.fail(Problem::Incomplete, "unterminated item");
                return std::nullopt;
            }
            cursor.seek(cursor.position() + i);
            return std::string(text.substr(0, i));
        }
    };

} // namespace terila::commands
