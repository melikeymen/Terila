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
    const auto nodes = protocol::builtin_command_tree(14, 3, 6, 20);
    ASSERT_EQ(nodes.size(), 28u);
    EXPECT_EQ(nodes[0].kind, protocol::CommandNode::Root);
    EXPECT_EQ(nodes[0].children, (std::vector<std::int32_t>{ 1, 7, 14 }));
    EXPECT_EQ(nodes[1].name, "give");
    EXPECT_EQ(nodes[1].children, (std::vector<std::int32_t>{ 2, 4 }));
    EXPECT_EQ(nodes[2].name, "item");
    EXPECT_EQ(nodes[2].parser, 14);   // minecraft:item_stack
    EXPECT_EQ(nodes[3].name, "count");
    EXPECT_EQ(nodes[3].parser, 3);    // brigadier:integer
    EXPECT_TRUE(nodes[3].executable);

    const auto bytes = protocol::write_command_tree(nodes, 0);
    ASSERT_GT(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 28);     // twenty eight nodes
    EXPECT_EQ(bytes[1], 0x00);   // root flags
    EXPECT_EQ(bytes[2], 3);      // give, execute and time
    EXPECT_EQ(bytes[3], 1);
}

TEST(CommandTreeTest, TimeIsInTheTreeWithItsKeywords) {
    const auto nodes = protocol::builtin_command_tree(14, 3, 6, 20);

    // /time offers set, add and query right away, so the client lists it after "/".
    EXPECT_EQ(nodes[14].kind, protocol::CommandNode::Literal);
    EXPECT_EQ(nodes[14].name, "time");
    EXPECT_EQ(nodes[14].children, (std::vector<std::int32_t>{ 15, 23, 25 }));

    EXPECT_EQ(nodes[15].name, "set");
    ASSERT_EQ(nodes[15].children.size(), 7u);
    EXPECT_EQ(nodes[16].name, "day");
    EXPECT_TRUE(nodes[16].executable);
    EXPECT_EQ(nodes[19].name, "night");
    EXPECT_TRUE(nodes[19].executable);
    EXPECT_EQ(nodes[22].name, "ticks");
    EXPECT_EQ(nodes[22].parser, 3);   // brigadier:integer
    EXPECT_TRUE(nodes[22].executable);

    EXPECT_EQ(nodes[23].name, "add");
    EXPECT_EQ(nodes[24].name, "ticks");
    EXPECT_TRUE(nodes[24].executable);

    EXPECT_EQ(nodes[25].name, "query");
    EXPECT_EQ(nodes[26].name, "daytime");
    EXPECT_EQ(nodes[27].name, "gametime");
    EXPECT_TRUE(nodes[26].executable);
    EXPECT_TRUE(nodes[27].executable);
}

TEST(CommandTreeTest, ExecuteOffersRunAndAsBranches) {
    const auto nodes = protocol::builtin_command_tree(14, 3, 6, 20);

    // /execute
    EXPECT_EQ(nodes[7].kind, protocol::CommandNode::Literal);
    EXPECT_EQ(nodes[7].name, "execute");
    EXPECT_EQ(nodes[7].children, (std::vector<std::int32_t>{ 8, 10 }));

    // /execute run <command>
    EXPECT_EQ(nodes[8].name, "run");
    EXPECT_EQ(nodes[8].children, (std::vector<std::int32_t>{ 9 }));
    EXPECT_EQ(nodes[9].name, "command");
    EXPECT_EQ(nodes[9].parser, 20);   // minecraft:message takes the rest of the line
    EXPECT_TRUE(nodes[9].executable);

    // /execute as <targets> run <command>
    EXPECT_EQ(nodes[10].name, "as");
    EXPECT_EQ(nodes[10].children, (std::vector<std::int32_t>{ 11 }));
    EXPECT_EQ(nodes[11].name, "targets");
    EXPECT_EQ(nodes[11].parser, 6);   // minecraft:entity
    ASSERT_EQ(nodes[11].properties.size(), 1u);
    EXPECT_EQ(nodes[11].properties[0], 0x02);   // players only
    EXPECT_EQ(nodes[12].name, "run");
    EXPECT_EQ(nodes[12].children, (std::vector<std::int32_t>{ 13 }));
    EXPECT_EQ(nodes[13].name, "command");
    EXPECT_TRUE(nodes[13].executable);

    // The two `run` literals are separate nodes: a Brigadier node has one parent, which
    // EveryChildIndexIsInRangeAndUsedOnce checks across the whole tree.
    EXPECT_EQ(nodes[8].name, nodes[12].name);
    EXPECT_NE(nodes[8].children, nodes[12].children);
}

TEST(CommandTreeTest, GiveAcceptsATargetSelectorBeforeTheItem) {
    const auto nodes = protocol::builtin_command_tree(14, 3, 6, 20);

    // /give @a <item> [count]
    EXPECT_EQ(nodes[4].name, "targets");
    EXPECT_EQ(nodes[4].parser, 6);   // minecraft:entity
    ASSERT_EQ(nodes[4].properties.size(), 1u);
    EXPECT_EQ(nodes[4].properties[0], 0x02);   // several players
    EXPECT_EQ(nodes[4].children, (std::vector<std::int32_t>{ 5 }));
    EXPECT_EQ(nodes[5].name, "item");
    EXPECT_EQ(nodes[5].parser, 14);
    EXPECT_EQ(nodes[6].name, "count");
    EXPECT_TRUE(nodes[5].executable);
    EXPECT_TRUE(nodes[6].executable);
}

TEST(CommandTreeTest, EveryChildIndexIsInRangeAndUsedOnce) {
    const auto nodes = protocol::builtin_command_tree(14, 3, 6, 20);

    std::vector<int> parents(nodes.size(), 0);
    for (const auto& node : nodes) {
        for (const auto child : node.children) {
            ASSERT_GE(child, 0);
            ASSERT_LT(static_cast<std::size_t>(child), nodes.size());
            ++parents[static_cast<std::size_t>(child)];
        }
    }
    // A Brigadier node has exactly one parent, so every node but the root is entered once.
    EXPECT_EQ(parents[0], 0);
    for (std::size_t i = 1; i < parents.size(); ++i) {
        EXPECT_EQ(parents[i], 1) << "node " << i;
    }
}

TEST(CommandTreeTest, IntegerPropertiesCarryTheMinimum) {
    const auto properties = protocol::integer_properties(1, false, 0);
    ASSERT_EQ(properties.size(), 5u);
    EXPECT_EQ(properties[0], 0x01);   // minimum only
    EXPECT_EQ(properties[1], 0x00);
    EXPECT_EQ(properties[2], 0x00);
    EXPECT_EQ(properties[3], 0x00);
    EXPECT_EQ(properties[4], 0x01);
}

TEST(CommandTreeTest, RejectsAVeryLongArgumentName) {
    std::vector<protocol::CommandNode> nodes(2);
    nodes[1].kind = protocol::CommandNode::Literal;
    nodes[1].name = std::string(40000, 'x');

    EXPECT_TRUE(protocol::write_command_tree(nodes, 0).empty());
}
