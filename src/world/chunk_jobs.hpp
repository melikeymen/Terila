#pragma once

// Streaming one view distance is over a thousand chunks and a single generate() call costs
// several milliseconds, so generating them one after another on the io thread is what makes
// a player stare at "Loading terrain" for ten seconds. A chunk is a pure function of the
// seed and its coordinate, so a batch can be produced on a small pool while the frames are
// still being built here.

#include "world/chunk.hpp"
#include "world/dimension_kind.hpp"
#include "world/world_generator.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace terila::world {

    // The generated column and the payload a fresh generation streams. Both come out of the
    // same pass on a worker, so the io thread only does the per-player bookkeeping.
    struct PreparedChunk {
        Chunk chunk;
        std::vector<std::uint8_t> encoded;
    };

    class ChunkJobs {
    public:
        static ChunkJobs& shared();

        // The worker count the pool starts with. Set once during startup, before the first
        // chunk is prepared; a pool that already exists keeps the threads it has.
        static void set_thread_limit(unsigned threads) noexcept;

        std::size_t threads() const noexcept { return workers_.size(); }

        // Generated and encoded off the calling thread; the result is ready in roughly the
        // cost of one chunk divided by the number of workers. `on_done` runs on the worker
        // the moment the task finishes, which is what lets a waiting stream wake up instead
        // of polling its future once a tick.
        // `cancelled` is asked once, on the worker, before the column is generated: a player
        // who logs out leaves a whole view distance queued, and generating it would only
        // hold up the next player's chunks.
        std::future<PreparedChunk> prepare(DimensionKind kind, std::int32_t cx, std::int32_t cz,
            std::function<void()> on_done = {}, std::function<bool()> cancelled = {});

        ChunkJobs(const ChunkJobs&) = delete;
        ChunkJobs& operator=(const ChunkJobs&) = delete;

    private:
        ChunkJobs();
        ~ChunkJobs();

        void work();

        std::mutex mutex_;
        std::condition_variable wake_;
        std::deque<std::packaged_task<PreparedChunk()>> queue_;
        std::vector<std::thread> workers_;
        bool stopping_ = false;
    };

} // namespace terila::world
