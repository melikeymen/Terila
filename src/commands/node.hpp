#pragma once

#include "commands/origin.hpp"
#include "commands/arguments/parser.hpp"
#include "commands/arguments/value.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace terila::commands {

    struct Exec;
    struct RunScope;
    class Registry;

    struct Result {
        std::int32_t count = 0;
        bool ok = true;
    };

    using Executor = std::function<Result(Exec&)>;

    // One node of the grammar. A literal matches its name, an argument hands the cursor to
    // its parser; both can carry children, and a node with an executor can end the line.
    struct Node {
        std::string name;
        bool is_argument = false;
        bool executable = false;
        Parser parser{};
        Capability need = Capability::Everyone;
        std::vector<std::unique_ptr<Node>> children;
        Executor run;

        // Children that exist only in the packet. A command can parse one greedy argument
        // here and still tell the client which words it starts with, so what a player sees
        // while typing is not a single placeholder.
        std::vector<std::unique_ptr<Node>> wire_children;

        Node* find_literal(std::string_view word) const {
            for (const auto& child : children) {
                if (!child->is_argument && child->name == word) {
                    return child.get();
                }
            }
            return nullptr;
        }

        Node& literal(std::string name, Capability need = Capability::Everyone) {
            if (Node* existing = find_literal(name)) {
                return *existing;
            }
            auto child = std::make_unique<Node>();
            child->name = std::move(name);
            child->need = need;
            children.push_back(std::move(child));
            return *children.back();
        }

        Node& argument(std::string name, Parser parser,
            Capability need = Capability::Everyone) {
            auto child = std::make_unique<Node>();
            child->name = std::move(name);
            child->is_argument = true;
            child->parser = std::move(parser);
            child->need = need;
            children.push_back(std::move(child));
            return *children.back();
        }

        Node& wire_literal(std::string name) {
            auto child = std::make_unique<Node>();
            child->name = std::move(name);
            wire_children.push_back(std::move(child));
            return *wire_children.back();
        }

        Node& wire_argument(std::string name, Parser parser) {
            auto child = std::make_unique<Node>();
            child->name = std::move(name);
            child->is_argument = true;
            child->parser = std::move(parser);
            wire_children.push_back(std::move(child));
            return *wire_children.back();
        }

        Node& executes(Executor executor) {
            executable = true;
            run = std::move(executor);
            return *this;
        }
    };

    // What an executor sees: who runs it, the values the line bound, the registry for a
    // nested command and the scope shared by the whole chain.
    struct Exec {
        Origin& origin;
        const Args& args;
        Registry& registry;
        RunScope& scope;
    };

    // Guards shared by every nested command of one line.
    struct RunScope {
        static constexpr int kMaxDepth = 8;
        static constexpr int kBudget = 65536;

        int depth = 0;
        int remaining = kBudget;
    };

} // namespace terila::commands
