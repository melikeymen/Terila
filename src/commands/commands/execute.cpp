#include "commands/registry.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace terila::commands {

    namespace {

        // The modifier chain is read left to right and applied to a working origin; `run`
        // hands what is left to the registry. Every fan-out shares one scope, so a chain can
        // widen the work but never the nesting depth.
        Result run_chain(Exec& exec, Origin origin, std::string_view text, std::size_t start);

        std::string_view next_word(Cursor& cursor) {
            return cursor.peek();
        }

        Result run_chain(Exec& exec, Origin origin, std::string_view text, std::size_t start) {
            Cursor cursor(text);
            cursor.seek(start);

            while (true) {
                cursor.skip_space();
                std::string_view keyword = next_word(cursor);
                if (keyword.empty()) {
                    origin.say("Expected 'run' to finish the chain");
                    return Result{ 0, false };
                }

                if (cursor.literal("run")) {
                    const auto tail = cursor.greedy();
                    if (tail.empty()) {
                        origin.say("Expected a command after 'run'");
                        return Result{ 0, false };
                    }
                    return exec.registry.run(origin, tail, exec.scope);
                }

                // `in <dimension>` does not move the source, it only says which world the
                // rest of the chain reads its coordinates from and teleports into.
                if (cursor.literal("in")) {
                    auto value = Parser::resource_location().read(cursor);
                    if (!value) {
                        origin.say("Expected a dimension");
                        return Result{ 0, false };
                    }
                    const auto* name = std::get_if<std::string>(&*value);
                    if (name == nullptr) {
                        origin.say("Expected a dimension");
                        return Result{ 0, false };
                    }
                    origin.dimension = *name;
                    continue;
                }

                if (cursor.literal("as") || cursor.literal("at")) {
                    const bool moves_position = keyword == "at";
                    auto value = Parser::players().read(cursor);
                    if (!value) {
                        origin.say("Expected a target selector");
                        return Result{ 0, false };
                    }
                    const auto* selector = std::get_if<Selector>(&*value);
                    std::string error;
                    const auto targets = targets_of(*selector, origin, error);
                    if (targets.empty()) {
                        origin.say(error.empty() ? "No entity was found" : error);
                        return Result{ 0, false };
                    }

                    Result total{ 0, true };
                    const std::size_t resume = cursor.position();
                    for (const auto& target : targets) {
                        Origin next = origin;
                        next.entity = target.entity;
                        next.name = target.name;
                        if (moves_position) {
                            next.x = target.x;
                            next.y = target.y;
                            next.z = target.z;
                            next.yaw = target.yaw;
                            next.pitch = target.pitch;
                        }
                        const Result part = run_chain(exec, next, text, resume);
                        total.count += part.count;
                        total.ok = total.ok && part.ok;
                    }
                    return total;
                }

                if (cursor.literal("positioned")) {
                    if (cursor.literal("as")) {
                        auto value = Parser::players().read(cursor);
                        if (!value) {
                            origin.say("Expected a target selector");
                            return Result{ 0, false };
                        }
                        const auto* selector = std::get_if<Selector>(&*value);
                        std::string error;
                        const auto targets = targets_of(*selector, origin, error);
                        if (targets.empty()) {
                            origin.say(error.empty() ? "No entity was found" : error);
                            return Result{ 0, false };
                        }
                        const std::size_t resume = cursor.position();
                        Result total{ 0, true };
                        for (const auto& target : targets) {
                            Origin next = origin;
                            next.x = target.x;
                            next.y = target.y;
                            next.z = target.z;
                            const Result part = run_chain(exec, next, text, resume);
                            total.count += part.count;
                            total.ok = total.ok && part.ok;
                        }
                        return total;
                    }

                    auto value = Parser::position().read(cursor);
                    if (!value) {
                        origin.say("Expected a position");
                        return Result{ 0, false };
                    }
                    const auto* position = std::get_if<Position>(&*value);
                    resolve_position(*position, origin.x, origin.y, origin.z, origin.yaw,
                        origin.pitch, origin.x, origin.y, origin.z);
                    continue;
                }

                if (cursor.literal("rotated")) {
                    if (cursor.literal("as")) {
                        auto value = Parser::players().read(cursor);
                        if (!value) {
                            origin.say("Expected a target selector");
                            return Result{ 0, false };
                        }
                        const auto* selector = std::get_if<Selector>(&*value);
                        std::string error;
                        const auto targets = targets_of(*selector, origin, error);
                        if (targets.empty()) {
                            origin.say(error.empty() ? "No entity was found" : error);
                            return Result{ 0, false };
                        }
                        const std::size_t resume = cursor.position();
                        Result total{ 0, true };
                        for (const auto& target : targets) {
                            Origin next = origin;
                            next.yaw = target.yaw;
                            next.pitch = target.pitch;
                            const Result part = run_chain(exec, next, text, resume);
                            total.count += part.count;
                            total.ok = total.ok && part.ok;
                        }
                        return total;
                    }

                    auto value = Parser::rotation().read(cursor);
                    if (!value) {
                        origin.say("Expected a rotation");
                        return Result{ 0, false };
                    }
                    const auto* rotation = std::get_if<RotationValue>(&*value);
                    origin.yaw = rotation->yaw_relative ? origin.yaw + rotation->yaw
                                                        : rotation->yaw;
                    origin.pitch = rotation->pitch_relative ? origin.pitch + rotation->pitch
                                                            : rotation->pitch;
                    continue;
                }

                if (cursor.literal("align")) {
                    const auto axes = cursor.word();
                    if (!axes) {
                        origin.say("Expected the axes to align on");
                        return Result{ 0, false };
                    }
                    for (const char axis : *axes) {
                        switch (axis) {
                        case 'x': origin.x = std::floor(origin.x); break;
                        case 'y': origin.y = std::floor(origin.y); break;
                        case 'z': origin.z = std::floor(origin.z); break;
                        default:
                            origin.say(std::string("Unknown axis '") + axis + "'");
                            return Result{ 0, false };
                        }
                    }
                    continue;
                }

                origin.say("Unsupported execute modifier '" + std::string(keyword) + "'");
                return Result{ 0, false };
            }
        }

        // The chain is read here as one greedy argument, but the client builds the line from
        // the packet tree before it sends it and will not offer a command as a single
        // argument. So every branch is spelled out down to a phrase that takes whatever
        // continues the chain, which is also the only place the client needs to type.
        Node& wire_tail(Node& node) {
            Node& rest = node.wire_argument("command", Parser::greedy());
            rest.executable = true;
            return rest;
        }

        Node& wire_targets(Node& node) {
            return wire_tail(node.wire_argument("targets", Parser::players()));
        }

        Result run_execute(Exec& exec) {
            const auto* text = exec.args.get<std::string>("arguments");
            if (text == nullptr) {
                return Result{ 0, false };
            }
            return run_chain(exec, exec.origin, *text, 0);
        }

    } // namespace

    void install_execute(Registry& registry) {
        Node& execute = registry.root().literal("execute");
        execute.argument("arguments", Parser::greedy()).executes(run_execute);

        wire_tail(execute.wire_literal("run"));
        wire_targets(execute.wire_literal("as"));
        wire_targets(execute.wire_literal("at"));

        Node& in = execute.wire_literal("in");
        wire_tail(in.wire_argument("dimension", Parser::resource_location()));

        Node& positioned = execute.wire_literal("positioned");
        wire_targets(positioned.wire_literal("as"));
        wire_tail(positioned.wire_argument("pos", Parser::position()));

        Node& rotated = execute.wire_literal("rotated");
        wire_targets(rotated.wire_literal("as"));
        wire_tail(rotated.wire_argument("rotation", Parser::rotation()));

        wire_tail(execute.wire_literal("align").wire_argument("axes", Parser::text()));
    }

} // namespace terila::commands
