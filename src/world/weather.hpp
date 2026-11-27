#pragma once

#include <algorithm>
#include <cstdint>

namespace terila::world {

    // Rain and thunder last as long as a command says they do; the tick loop clears them
    // when the time runs out. The level a client draws is therefore always 0 or 1.
    class Weather {
    public:
        enum Kind : int {
            Clear = 0,
            Rain = 1,
            Thunder = 2
        };

        Kind kind() const { return kind_; }
        bool raining() const { return kind_ != Clear; }
        bool thundering() const { return kind_ == Thunder; }
        float rain_level() const { return raining() ? 1.0f : 0.0f; }
        float thunder_level() const { return thundering() ? 1.0f : 0.0f; }
        // Ticks left before the weather clears itself, 0 when it already has.
        std::int64_t remaining() const { return remaining_; }

        void set(Kind kind, std::int64_t ticks) {
            kind_ = kind;
            remaining_ = kind == Clear ? 0 : std::max<std::int64_t>(ticks, 1);
        }

        // One tick of the duration. True on the tick the weather ran out, which is when
        // the clients have to be told.
        bool tick() {
            if (kind_ == Clear) {
                return false;
            }
            if (--remaining_ > 0) {
                return false;
            }
            kind_ = Clear;
            remaining_ = 0;
            return true;
        }

    private:
        Kind kind_ = Clear;
        std::int64_t remaining_ = 0;
    };

} // namespace terila::world
