#include "world/chunk_jobs.hpp"

#include "world/chunk_encoder.hpp"
#include "world/world_context.hpp"

#include <algorithm>
#include <utility>

namespace terila::world {

    namespace {

        // Two cores keep the io thread and the tick loop theirs: a wider pool only takes
        // time away from the connection that is waiting for the batch.
        unsigned worker_count() {
            const unsigned hardware = std::thread::hardware_concurrency();
            return std::clamp(hardware == 0 ? 2u : hardware, 2u, 8u);
        }

    } // namespace

    ChunkJobs& ChunkJobs::shared() {
        static ChunkJobs jobs;
        return jobs;
    }

    ChunkJobs::ChunkJobs() {
        const unsigned count = worker_count();
        workers_.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            workers_.emplace_back([this] { work(); });
        }
    }

    ChunkJobs::~ChunkJobs() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    std::future<PreparedChunk> ChunkJobs::prepare(std::int32_t cx, std::int32_t cz) {
        // The shared generator is read-only after its init(), and the noise caches are
        // per-thread, so several chunks of the same world generate side by side safely.
        std::packaged_task<PreparedChunk()> task([cx, cz] {
            PreparedChunk prepared;
            prepared.chunk = WorldContext::shared().generator().generate(cx, cz);
            prepared.encoded = encode::encode(prepared.chunk);
            return prepared;
        });

        auto done = task.get_future();
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        wake_.notify_one();
        return done;
    }

    void ChunkJobs::work() {
        while (true) {
            std::packaged_task<PreparedChunk()> task;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) {
                        return;
                    }
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }

} // namespace terila::world
