#pragma once

#include "protocol/primitives.hpp"
#include "protocol/string.hpp"
#include "protocol/varint.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace terila::protocol {

    // ClientboundCommandsPacket, 26.3: flags (kind in bits 0-1, executable 0x04,
    // redirect 0x08, suggestions 0x10, restricted 0x20), children, name, parser id and
    // its property bytes, then the root index.
    struct CommandNode {
        enum Kind : std::uint8_t { Root = 0, Literal = 1, Argument = 2 };

        std::uint8_t kind = Literal;
        bool executable = false;
        std::vector<std::int32_t> children;
        std::string name;
        std::int32_t parser = -1;
        std::vector<std::uint8_t> properties;
    };

    // The client rejects a tree that cannot be walked into a Brigadier graph (it throws
    // "Server sent an impossible command tree"), so the nodes are built here instead of
    // being copied from a dump: the shape is known to be acyclic and fully connected.
    inline std::vector<std::uint8_t> write_command_tree(const std::vector<CommandNode>& nodes,
        std::int32_t root) {
        std::vector<std::uint8_t> out;
        write_varint(out, static_cast<std::int32_t>(nodes.size()));

        for (const auto& node : nodes) {
            std::uint8_t flags = static_cast<std::uint8_t>(node.kind & 0x03);
            if (node.executable) {
                flags |= 0x04;
            }
            out.push_back(flags);

            write_varint(out, static_cast<std::int32_t>(node.children.size()));
            for (const auto child : node.children) {
                write_varint(out, child);
            }

            if (node.kind >= CommandNode::Literal) {
                if (!write_string(out, node.name)) {
                    return {};
                }
            }
            if (node.kind == CommandNode::Argument) {
                write_varint(out, node.parser);
                out.insert(out.end(), node.properties.begin(), node.properties.end());
            }
        }

        write_varint(out, root);
        return out;
    }

    // brigadier:string properties: 0 single word, 1 quoted phrase, 2 greedy phrase.
    inline std::vector<std::uint8_t> greedy_string_properties() {
        std::vector<std::uint8_t> properties;
        write_varint(properties, 2);
        return properties;
    }

    // brigadier:integer properties: a flag byte (bit 0 min, bit 1 max) then the bounds.
    inline std::vector<std::uint8_t> integer_properties(std::int32_t min, bool has_max,
        std::int32_t max) {
        std::vector<std::uint8_t> properties;
        properties.push_back(static_cast<std::uint8_t>(has_max ? 0x03 : 0x01));
        terila::protocol::write_int32(properties, min);
        if (has_max) {
            terila::protocol::write_int32(properties, max);
        }
        return properties;
    }

    // minecraft:entity properties: one flags byte, 0x01 single entity only and 0x02
    // players only. /give takes several players, so the selector is players-only.
    inline std::vector<std::uint8_t> players_only_properties() {
        return std::vector<std::uint8_t>{ 0x02 };
    }

    // The commands this server runs, in the order the login handler sends them:
    //   /give <targets> <item> [count]
    //   /give <item> [count]
    //   /execute [as <targets>] run <command>
    // The item argument uses minecraft:item_stack, the parser /give uses in the game, so
    // the client completes item ids and validates the component patch it is given, and
    // the target argument uses minecraft:entity so @a, @p, @s and a player name parse.
    // The executed command is a minecraft:message argument: it takes the rest of the line,
    // which is what the client needs to accept "/execute run give stone" as one argument.
    //
    // Branches are repeated on purpose: a Brigadier node has one parent, so the two shapes
    // of /give, and the two `run` literals, cannot share the same subtree indices.
    inline std::vector<CommandNode> builtin_command_tree(std::int32_t item_stack_parser,
        std::int32_t integer_parser, std::int32_t entity_parser, std::int32_t message_parser) {
        std::vector<CommandNode> nodes(14);

        // The client casts the resolved root to RootCommandNode, so node 0 must be the
        // stub-less root type; as a literal the cast fails after a successful decode.
        nodes[0].kind = CommandNode::Root;
        nodes[0].children = { 1, 7 };

        nodes[1].kind = CommandNode::Literal;
        nodes[1].name = "give";
        nodes[1].children = { 2, 4 };

        // /give <item> [count]
        nodes[2].kind = CommandNode::Argument;
        nodes[2].name = "item";
        nodes[2].parser = item_stack_parser;
        nodes[2].children = { 3 };
        nodes[2].executable = true;

        nodes[3].kind = CommandNode::Argument;
        nodes[3].name = "count";
        nodes[3].parser = integer_parser;
        nodes[3].properties = integer_properties(1, false, 0);
        nodes[3].executable = true;

        // /give <targets> <item> [count]
        nodes[4].kind = CommandNode::Argument;
        nodes[4].name = "targets";
        nodes[4].parser = entity_parser;
        nodes[4].properties = players_only_properties();
        nodes[4].children = { 5 };

        nodes[5].kind = CommandNode::Argument;
        nodes[5].name = "item";
        nodes[5].parser = item_stack_parser;
        nodes[5].children = { 6 };
        nodes[5].executable = true;

        nodes[6].kind = CommandNode::Argument;
        nodes[6].name = "count";
        nodes[6].parser = integer_parser;
        nodes[6].properties = integer_properties(1, false, 0);
        nodes[6].executable = true;

        // /execute [as <targets>] run <command>
        nodes[7].kind = CommandNode::Literal;
        nodes[7].name = "execute";
        nodes[7].children = { 8, 10 };

        // /execute run <command>
        nodes[8].kind = CommandNode::Literal;
        nodes[8].name = "run";
        nodes[8].children = { 9 };

        nodes[9].kind = CommandNode::Argument;
        nodes[9].name = "command";
        nodes[9].parser = message_parser;
        nodes[9].executable = true;

        // /execute as <targets> run <command>
        nodes[10].kind = CommandNode::Literal;
        nodes[10].name = "as";
        nodes[10].children = { 11 };

        nodes[11].kind = CommandNode::Argument;
        nodes[11].name = "targets";
        nodes[11].parser = entity_parser;
        nodes[11].properties = players_only_properties();
        nodes[11].children = { 12 };

        nodes[12].kind = CommandNode::Literal;
        nodes[12].name = "run";
        nodes[12].children = { 13 };

        nodes[13].kind = CommandNode::Argument;
        nodes[13].name = "command";
        nodes[13].parser = message_parser;
        nodes[13].executable = true;

        return nodes;
    }

} // namespace terila::protocol
