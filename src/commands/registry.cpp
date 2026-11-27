#include "commands/registry.hpp"

#include "commands/cursor.hpp"
#include "commands/arguments/text.hpp"
#include "protocol/varint.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <string>
#include <utility>

namespace terila::commands {

    namespace {

        // Keeps the failure that got furthest into the line: that is the branch the player
        // was actually typing.
        void note(Diagnostic& best, Problem kind, std::size_t offset, std::string message) {
            if (best.message.empty() || offset >= best.offset) {
                best.kind = kind;
                best.offset = offset;
                best.message = std::move(message);
            }
        }

        void report(const Origin& origin, const Diagnostic& diagnostic,
            std::string_view line) {
            if (diagnostic.message.empty()) {
                origin.say("Unknown or incomplete command");
                return;
            }
            if (diagnostic.offset < line.size()) {
                origin.say(std::format("{} (at position {})", diagnostic.message,
                    diagnostic.offset + 1));
            }
            else {
                origin.say(diagnostic.message);
            }
        }

        bool allowed(const Node& node, const Origin& origin) {
            return static_cast<std::uint8_t>(origin.capability) >=
                static_cast<std::uint8_t>(node.need);
        }

    } // namespace

    Registry::Registry(const data::RegistryData& data, const item::ComponentIds& components)
        : data_(data), components_(components) {
        root_.name = "";
    }

    void Registry::install_all() {
        install_give(*this);
        install_time(*this);
        install_execute(*this);
        install_gamemode(*this);
        install_teleport(*this);
        install_summon(*this);
        install_message(*this);
        install_kill(*this);
        install_query(*this);
        install_say(*this);
        install_edit(*this);
        install_world_state(*this);
        install_damage(*this);
        install_clone(*this);
        install_spawn(*this);
        install_clear(*this);
        install_server_control(*this);
    }

    namespace {

        // One executable path of a command, arguments written the way /help writes them.
        void collect_usage(const Node& node, const std::string& path,
            std::vector<std::string>& out, std::size_t limit) {
            if (out.size() >= limit) {
                return;
            }
            if (node.executable) {
                out.push_back(path);
            }
            for (const auto& child : node.children) {
                if (out.size() >= limit) {
                    return;
                }
                if (child->is_argument) {
                    collect_usage(*child, path + " <" + child->name + ">", out, limit);
                }
                else {
                    collect_usage(*child, path + " " + child->name, out, limit);
                }
            }
        }

    } // namespace

    std::vector<std::string> Registry::command_names() const {
        std::vector<std::string> names;
        for (const auto& child : root_.children) {
            if (!child->is_argument) {
                names.push_back(child->name);
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    std::vector<std::string> Registry::usage_lines(std::string_view command,
        std::size_t limit) const {
        std::vector<std::string> lines;
        for (const auto& child : root_.children) {
            if (child->is_argument || !equal_ignore_case(child->name, command)) {
                continue;
            }
            collect_usage(*child, "/" + child->name, lines, limit);
            break;
        }
        return lines;
    }

    Parser Registry::item_parser() const {
        Parser parser = Parser::item_stack();
        parser.item_ids = &data_.item_ids;
        parser.components = &components_;
        return parser;
    }

    Parser Registry::block_state_parser() const {
        Parser parser = Parser::block_state();
        parser.block_states = &data_.block_states;
        parser.block_state_ids = &data_.block_state_ids;
        return parser;
    }

    std::int32_t Registry::parser_id(std::string_view wire, std::int32_t fallback) const {
        const auto found = data_.command_argument_types.find(std::string(wire));
        return found == data_.command_argument_types.end() ? fallback : found->second;
    }

    // Tries every child of `node` from the cursor, in declaration order, and backtracks to
    // the same position when a branch does not reach an executable node.
    bool match_from(const Node& node, Cursor& cursor, Args& args, const Origin& origin,
        Diagnostic& best, const Node*& out) {
        cursor.skip_space();
        if (cursor.at_end()) {
            if (node.executable) {
                out = &node;
                return true;
            }
            note(best, Problem::Incomplete, cursor.position(), "incomplete command");
            return false;
        }

        for (const auto& child : node.children) {
            if (!allowed(*child, origin)) {
                continue;
            }

            const std::size_t saved_position = cursor.position();
            const std::size_t saved_args = args.values.size();
            cursor.clear_diagnostic();

            bool matched = false;
            if (child->is_argument) {
                if (auto value = child->parser.read(cursor)) {
                    args.values.emplace_back(child->name, std::move(*value));
                    matched = true;
                }
            }
            else {
                matched = cursor.literal(child->name);
            }

            if (matched) {
                const Node* found = nullptr;
                if (match_from(*child, cursor, args, origin, best, found)) {
                    out = found;
                    return true;
                }
                if (!cursor.diagnostic().message.empty()) {
                    note(best, cursor.diagnostic().kind, cursor.diagnostic().offset,
                        cursor.diagnostic().message);
                }
            }

            cursor.seek(saved_position);
            args.values.resize(saved_args);
        }

        note(best, Problem::Invalid, cursor.position(), "unknown or incomplete command");
        return false;
    }

    Result Registry::run(Origin& origin, std::string_view line, RunScope& scope) {
        if (scope.depth >= RunScope::kMaxDepth) {
            origin.say("Too many nested commands");
            return Result{ 0, false };
        }
        if (scope.remaining <= 0) {
            origin.say("Too many commands in one tick");
            return Result{ 0, false };
        }

        Cursor cursor(line);
        Args args;
        Diagnostic best;
        const Node* node = nullptr;

        ++scope.depth;
        const bool matched = match_from(root_, cursor, args, origin, best, node);
        --scope.depth;

        if (!matched || node == nullptr || !node->run) {
            report(origin, best, line);
            return Result{ 0, false };
        }

        --scope.remaining;
        Exec exec{ origin, args, *this, scope };
        return node->run(exec);
    }

    std::vector<protocol::CommandNode> Registry::wire_tree() const {
        std::vector<protocol::CommandNode> nodes;

        // Depth-first with an index fixed before the children are visited, so a child
        // always refers to a node that exists.
        const std::function<std::int32_t(const Node&, bool)> emit =
            [&](const Node& node, bool is_root) -> std::int32_t {
            const auto index = static_cast<std::int32_t>(nodes.size());
            nodes.emplace_back();

            protocol::CommandNode built;
            built.kind = is_root ? protocol::CommandNode::Root
                                 : node.is_argument ? protocol::CommandNode::Argument
                                                    : protocol::CommandNode::Literal;
            built.executable = node.executable;
            built.name = node.name;
            built.restricted = node.need > Capability::Everyone;

            if (node.is_argument) {
                built.parser = parser_id(node.parser.wire, -1);
                if (built.parser < 0) {
                    nodes.pop_back();
                    return -1;
                }
                switch (node.parser.kind) {
                case ParserKind::Integer:
                case ParserKind::Real:
                    built.properties = protocol::integer_properties(
                        static_cast<std::int32_t>(node.parser.min),
                        node.parser.has_max,
                        static_cast<std::int32_t>(node.parser.max));
                    break;
                case ParserKind::Text:
                case ParserKind::GreedyText:
                    // Only the brigadier string carries a reading mode; a message argument
                    // is greedy by definition and its parser has no property bytes.
                    if (node.parser.wire == "brigadier:string") {
                        protocol::write_varint(built.properties, node.parser.text_mode);
                    }
                    break;
                case ParserKind::PlayerNames:
                case ParserKind::OnePlayer: {
                    std::uint8_t flags = 0;
                    if (node.parser.single) {
                        flags |= 0x01;
                    }
                    if (node.parser.players_only) {
                        flags |= 0x02;
                    }
                    built.properties.push_back(flags);
                    break;
                }
                default:
                    break;
                }
            }

            for (const auto& child : node.children) {
                const auto child_index = emit(*child, false);
                if (child_index >= 0) {
                    built.children.push_back(child_index);
                }
            }

            nodes[static_cast<std::size_t>(index)] = std::move(built);
            return index;
        };

        emit(root_, true);
        return nodes;
    }

    std::vector<Target> targets_of(const Selector& selector, const Origin& origin,
        std::string& error) {
        std::vector<Target> candidates;
        if (origin.game != nullptr) {
            // @e names every entity, so it reads the whole world; a player selector only
            // reads the list the hub keeps, which is also what a plain name resolves in.
            candidates = selector.marker == 'e' ? origin.game->entities()
                                                : origin.game->players();
        }
        if (candidates.empty()) {
            candidates.push_back(Target{ origin.entity, origin.name, origin.x, origin.y,
                origin.z });
        }

        const Target self{ origin.entity, origin.name, origin.x, origin.y, origin.z };
        auto matches = resolve(selector, self, candidates);
        if (matches.empty()) {
            error = "No entity was found matching '" + selector.text + "'";
        }
        return matches;
    }

    void resolve_position(const Position& position, double base_x, double base_y,
        double base_z, float yaw, float pitch, double& x, double& y, double& z) {
        if (position.mode == Position::Mode::Local) {
            constexpr double kPi = 3.14159265358979323846;
            const double yaw_radians = static_cast<double>(yaw) * kPi / 180.0;
            const double pitch_radians = static_cast<double>(pitch) * kPi / 180.0;

            // Local space: forward along the facing, left and up perpendicular to it.
            const double forward_x = -std::sin(yaw_radians) * std::cos(pitch_radians);
            const double forward_y = -std::sin(pitch_radians);
            const double forward_z = std::cos(yaw_radians) * std::cos(pitch_radians);
            const double left_x = -std::cos(yaw_radians);
            const double left_z = -std::sin(yaw_radians);

            x = base_x + forward_x * position.x + left_x * position.y;
            y = base_y + forward_y * position.x + position.y;
            z = base_z + forward_z * position.x + left_z * position.y;
            return;
        }

        x = position.relative_x ? base_x + position.x : position.x;
        y = position.relative_y ? base_y + position.y : position.y;
        z = position.relative_z ? base_z + position.z : position.z;
    }

} // namespace terila::commands
