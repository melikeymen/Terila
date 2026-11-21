#pragma once

// A process runs one world, so the generators and the chunk caches belong to the process and
// not to a connection: two players standing next to each other must not pay for the same
// column twice, and a player who steps into the nether keeps the overworld around for the
// next player who steps back out.

#include "world/dimension_kind.hpp"
#include "world/chunk_cache.hpp"
#include "world/world_generator.hpp"

#include <array>

namespace terila::world {

    class WorldContext {
    public:
        static WorldContext& shared() {
            static WorldContext context;
            return context;
        }

        WorldGenerator& generator(DimensionKind kind = DimensionKind::Overworld) noexcept {
            return generators_[static_cast<std::size_t>(dimension_index(kind))];
        }

        ChunkCache& chunks(DimensionKind kind = DimensionKind::Overworld) noexcept {
            return chunks_[static_cast<std::size_t>(dimension_index(kind))];
        }

    private:
        std::array<WorldGenerator, kDimensionCount> generators_;
        std::array<ChunkCache, kDimensionCount> chunks_;
    };

} // namespace terila::world
