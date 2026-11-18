#pragma once

// The overworld generator now lives in world/worldgen: one module per piece of the game's
// worldgen data (noise, density functions, climate parameters, material rules, ore veins and
// vegetation) plus a thin generator that binds them. This header keeps the old entry point
// and name for the callers that only want "the terrain generator".

#include "world/worldgen/generator.hpp"

namespace terila::world::terrain {

    using Generator = worldgen::Generator;

} // namespace terila::world::terrain
