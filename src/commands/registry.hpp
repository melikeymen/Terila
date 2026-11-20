#pragma once

#include "commands/node.hpp"
#include "commands/origin.hpp"
#include "data/registry_data.hpp"
#include "item/components.hpp"
#include "protocol/command_tree.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace terila::commands {

    // The grammar of every command this server runs, and the source of the tree the client
    // receives. One registry serves parsing, execution and the wire, so a command cannot
    // exist in one place and be missing from another.
    class Registry {
    public:
        Registry(const data::RegistryData& data, const item::ComponentIds& components);

        // Registers the built-in commands. Called once, before any line is parsed.
        void install_all();

        // Parses and runs one line, without the leading slash. Nested commands share the
        // scope, so /execute cannot deepen or widen without bound.
        Result run(Origin& origin, std::string_view line, RunScope& scope);

        std::vector<protocol::CommandNode> wire_tree() const;

        Node& root() { return root_; }
        const data::RegistryData& data() const { return data_; }
        const item::ComponentIds& components() const { return components_; }

        // The parsers that need registry data carry it with them.
        Parser item_parser() const;
        std::int32_t parser_id(std::string_view wire, std::int32_t fallback) const;

    private:
        const data::RegistryData& data_;
        const item::ComponentIds& components_;
        Node root_;
    };

    // Installed by command_*.cpp, one per command family.
    void install_give(Registry& registry);
    void install_time(Registry& registry);
    void install_execute(Registry& registry);
    void install_gamemode(Registry& registry);
    void install_teleport(Registry& registry);
    void install_summon(Registry& registry);
    void install_message(Registry& registry);

    // Resolves a selector against the players the origin can see, with the error the command
    // should report when nothing matched.
    std::vector<Target> targets_of(const Selector& selector, const Origin& origin,
        std::string& error);

    // Resolves an unresolved position against a base point. A relative coordinate adds to
    // the base, a local one is measured along the base's facing.
    void resolve_position(const Position& position, double base_x, double base_y,
        double base_z, float yaw, float pitch, double& x, double& y, double& z);

} // namespace terila::commands
