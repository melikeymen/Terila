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
        // The client hides a restricted node unless it believes it is an operator. One tree
        // is sent to everybody, which is what the game does.
        bool restricted = false;
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
            if (node.restricted) {
                flags |= 0x20;
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

    // brigadier:double properties: the same flag byte, but every bound is a full double.
    // Writing them as integers shifts every following byte by four, which the client
    // reports as a decode failure of the whole commands packet.
    inline std::vector<std::uint8_t> double_properties(double min, bool has_max, double max) {
        std::vector<std::uint8_t> properties;
        properties.push_back(static_cast<std::uint8_t>(has_max ? 0x03 : 0x01));
        terila::protocol::write_double(properties, min);
        if (has_max) {
            terila::protocol::write_double(properties, max);
        }
        return properties;
    }

} // namespace terila::protocol
