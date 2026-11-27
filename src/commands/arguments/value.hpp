#pragma once

#include "commands/arguments/selector.hpp"
#include "item/components.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace terila::commands {

    // A position as the line wrote it: `~` and `^` parts are kept unresolved because what
    // they mean depends on who runs the command and on who it is applied to.
    struct Position {
        enum class Mode { Absolute, Relative, Local };

        Mode mode = Mode::Absolute;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        bool relative_x = false;
        bool relative_y = false;
        bool relative_z = false;
    };

    struct RotationValue {
        float yaw = 0.0f;
        float pitch = 0.0f;
        bool yaw_relative = false;
        bool pitch_relative = false;
    };

    struct ItemStackValue {
        std::string id;
        item::ComponentsPtr components;
    };

    // A block written as `<id>` or `<id>[<property>=<value>,...]`, resolved to the state id
    // the world stores.
    struct BlockStateValue {
        std::int32_t id = -1;
        std::string name;
    };

    using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string,
        Position, RotationValue, Selector, ItemStackValue, BlockStateValue>;

    // The values a matched line bound, in the order the grammar declared them.
    struct Args {
        std::vector<std::pair<std::string, Value>> values;

        const Value* find(std::string_view name) const {
            for (const auto& [key, value] : values) {
                if (key == name) {
                    return &value;
                }
            }
            return nullptr;
        }

        template <typename T>
        const T* get(std::string_view name) const {
            const Value* value = find(name);
            return value == nullptr ? nullptr : std::get_if<T>(value);
        }
    };

} // namespace terila::commands
