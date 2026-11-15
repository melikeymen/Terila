#pragma once

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

    // The commands this server runs, in the order the login handler sends them:
    //   /give <item>      /enchant <text>
    // Both take the rest of the line, so a component patch such as
    // diamond_sword[minecraft:unbreakable={}] stays one argument.
    inline std::vector<CommandNode> builtin_command_tree(std::int32_t string_parser) {
        std::vector<CommandNode> nodes(5);

        // The client casts the resolved root to RootCommandNode, so node 0 must be the
        // stub-less root type; as a literal the cast fails after a successful decode.
        nodes[0].kind = CommandNode::Root;
        nodes[0].children = { 1, 2 };

        nodes[1].kind = CommandNode::Literal;
        nodes[1].name = "give";
        nodes[1].children = { 3 };

        nodes[2].kind = CommandNode::Literal;
        nodes[2].name = "enchant";
        nodes[2].children = { 4 };

        nodes[3].kind = CommandNode::Argument;
        nodes[3].name = "item";
        nodes[3].parser = string_parser;
        nodes[3].properties = greedy_string_properties();
        nodes[3].executable = true;

        nodes[4].kind = CommandNode::Argument;
        nodes[4].name = "text";
        nodes[4].parser = string_parser;
        nodes[4].properties = greedy_string_properties();
        nodes[4].executable = true;

        return nodes;
    }

} // namespace terila::protocol
