#include <gtest/gtest.h>

#include "protocol/command_tree.hpp"

#include <cstdint>
#include <vector>

namespace protocol = terila::protocol;

TEST(CommandTreeTest, EncodesCountRootIndexAndGreedyArguments) {
    std::vector<protocol::CommandNode> nodes(3);
    nodes[0].kind = protocol::CommandNode::Root;
    nodes[0].children = { 1 };

    nodes[1].kind = protocol::CommandNode::Literal;
    nodes[1].name = "give";
    nodes[1].children = { 2 };

    nodes[2].kind = protocol::CommandNode::Argument;
    nodes[2].name = "item";
    nodes[2].parser = 5;   // brigadier:string
    nodes[2].properties = protocol::greedy_string_properties();
    nodes[2].executable = true;

    const auto bytes = protocol::write_command_tree(nodes, 0);
    ASSERT_GT(bytes.size(), 6u);

    // varint node count, then the root index as a trailing varint.
    EXPECT_EQ(bytes.front(), 3);
    EXPECT_EQ(bytes.back(), 0);

    // The argument node closes the payload: parser id, then the greedy property
    // (varint 2), then the root index.
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[bytes.size() - 3], 5);   // brigadier:string
    EXPECT_EQ(bytes[bytes.size() - 2], 2);   // greedy phrase
}

TEST(CommandTreeTest, BuiltinTreeStartsWithARootNode) {
    const auto nodes = protocol::builtin_command_tree(5);
    ASSERT_EQ(nodes.size(), 5u);
    EXPECT_EQ(nodes[0].kind, protocol::CommandNode::Root);
    EXPECT_EQ(nodes[0].children, (std::vector<std::int32_t>{ 1, 2 }));
    EXPECT_EQ(nodes[1].name, "give");
    EXPECT_EQ(nodes[2].name, "enchant");
    EXPECT_EQ(nodes[3].parser, 5);
    EXPECT_TRUE(nodes[3].executable);

    const auto bytes = protocol::write_command_tree(nodes, 0);
    ASSERT_GT(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 5);      // five nodes
    EXPECT_EQ(bytes[1], 0x00);   // root flags
    EXPECT_EQ(bytes[2], 2);      // two children
    EXPECT_EQ(bytes[3], 1);
    EXPECT_EQ(bytes[4], 2);
}

TEST(CommandTreeTest, RejectsAVeryLongArgumentName) {
    std::vector<protocol::CommandNode> nodes(2);
    nodes[1].kind = protocol::CommandNode::Literal;
    nodes[1].name = std::string(40000, 'x');

    EXPECT_TRUE(protocol::write_command_tree(nodes, 0).empty());
}
