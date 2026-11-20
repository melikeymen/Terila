#include <gtest/gtest.h>

#include "commands/registry.hpp"
#include "protocol/command_tree.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace protocol = terila::protocol;

namespace {

    terila::data::RegistryData command_data() {
        terila::data::RegistryData data;
        data.command_argument_types = {
            { "brigadier:bool", 0 },
            { "brigadier:integer", 3 },
            { "brigadier:string", 5 },
            { "minecraft:entity", 6 },
            { "minecraft:item_stack", 14 },
            { "minecraft:vec3", 15 },
            { "minecraft:rotation", 16 },
            { "minecraft:gamemode", 17 },
            { "minecraft:message", 20 },
            { "minecraft:resource_location", 21 },
        };
        data.item_ids = { { "minecraft:stone", 1 } };
        return data;
    }

    terila::commands::Registry make_registry(const terila::data::RegistryData& data,
        const terila::item::ComponentIds& components) {
        terila::commands::Registry registry(data, components);
        registry.install_all();
        return registry;
    }

    const protocol::CommandNode* find_child(const std::vector<protocol::CommandNode>& nodes,
        const protocol::CommandNode& parent, const std::string& name) {
        for (const auto child : parent.children) {
            if (nodes[static_cast<std::size_t>(child)].name == name) {
                return &nodes[static_cast<std::size_t>(child)];
            }
        }
        return nullptr;
    }

} // namespace

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

TEST(CommandTreeTest, RootCarriesEveryBuiltInCommand) {
    const auto data = command_data();
    const terila::item::ComponentIds components;
    const auto registry = make_registry(data, components);

    const auto nodes = registry.wire_tree();
    ASSERT_FALSE(nodes.empty());
    EXPECT_EQ(nodes.front().kind, protocol::CommandNode::Root);

    for (const char* name : { "give", "time", "execute", "gamemode", "teleport", "tp",
             "summon", "msg", "tell", "w" }) {
        EXPECT_NE(find_child(nodes, nodes.front(), name), nullptr) << name;
    }
}

TEST(CommandTreeTest, EveryChildIndexIsInRangeAndUsedOnce) {
    const auto data = command_data();
    const terila::item::ComponentIds components;
    const auto registry = make_registry(data, components);
    const auto nodes = registry.wire_tree();

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

TEST(CommandTreeTest, TeleportCarriesItsPositionAndRotationArguments) {
    const auto data = command_data();
    const terila::item::ComponentIds components;
    const auto registry = make_registry(data, components);
    const auto nodes = registry.wire_tree();

    const auto* teleport = find_child(nodes, nodes.front(), "teleport");
    ASSERT_NE(teleport, nullptr);

    const auto* location = find_child(nodes, *teleport, "location");
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->kind, protocol::CommandNode::Argument);
    EXPECT_EQ(location->parser, 15);   // minecraft:vec3
    EXPECT_TRUE(location->executable);

    const auto* targets = find_child(nodes, *teleport, "targets");
    ASSERT_NE(targets, nullptr);
    ASSERT_EQ(targets->properties.size(), 1u);
    EXPECT_EQ(targets->properties[0], 0x02);   // players only

    const auto* rotation = find_child(nodes, *location, "rotation");
    EXPECT_EQ(rotation, nullptr);   // rotation hangs off the targets branch

    const auto* targets_location = find_child(nodes, *targets, "location");
    ASSERT_NE(targets_location, nullptr);
    EXPECT_NE(find_child(nodes, *targets_location, "rotation"), nullptr);

    const auto* tp = find_child(nodes, nodes.front(), "tp");
    ASSERT_NE(tp, nullptr);
    EXPECT_NE(find_child(nodes, *tp, "location"), nullptr);
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
