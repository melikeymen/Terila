#pragma once

// A process runs one world, so the generator and the chunk cache belong to the process and
// not to a connection: two players standing next to each other must not pay for the same
// column twice.

#include "world/chunk_cache.hpp"
#include "world/world_generator.hpp"

namespace terila::world {

    class WorldContext {
    public:
        static WorldContext& shared() {
            static WorldContext context;
            return context;
        }

        WorldGenerator& generator() noexcept { return generator_; }
        ChunkCache& chunks() noexcept { return chunks_; }

    private:
        WorldGenerator generator_;
        ChunkCache chunks_;
    };

} // namespace terila::world
