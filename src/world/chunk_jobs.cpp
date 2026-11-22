#include "world/chunk_jobs.hpp"

#include "world/chunk_encoder.hpp"
#include "world/world_context.hpp"

#include <algorithm>
#include <utility>

namespace terila::world {

    namespace {

        // Used when the runtime config does not name a worker count: one core stays with the
        // io thread, and two workers keep a batch from being serialised.
        unsigned default_worker_count() {
            const unsigned hardware = std::thread::hardware_concurrency();
            return std::clamp(hardware == 0 ? 2u : hardware - 1, 2u, 8u);
        }

        // Written once by set_thread_limit before the pool is constructed; a config that
        // leaves it at zero keeps the count derived from the machine.
        unsigned thread_limit = 0;

    } // namespace

    void ChunkJobs::set_thread_limit(unsigned threads) noexcept {
        thread_limit = std::clamp(threads, 1u, 64u);
    }

    ChunkJobs& ChunkJobs::shared() {
        static ChunkJobs jobs;
        return jobs;
    }

    ChunkJobs::ChunkJobs() {
        const unsigned count = thread_limit != 0 ? thread_limit : default_worker_count();
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

    std::future<PreparedChunk> ChunkJobs::prepare(DimensionKind kind, std::int32_t cx,
        std::int32_t cz, std::function<void()> on_done, std::function<bool()> cancelled) {
        // The shared generator is read-only after its init(), and the noise caches are
        // per-thread, so several chunks of the same world generate side by side safely.
        std::packaged_task<PreparedChunk()> task(
            [kind, cx, cz, done = std::move(on_done), cancel = std::move(cancelled)]() mutable {
                try {
                    if (cancel && cancel()) {
                        // The stream that asked for this column is gone; the caller drops the
                        // result, so the work is skipped instead of spent. A task still wakes
                        // its stream, which keeps the one-call-per-task rule intact.
                        if (done) {
                            done();
                        }
                        return PreparedChunk{};
                    }
                    PreparedChunk prepared;
                    prepared.chunk = WorldContext::shared().generator(kind).generate(cx, cz);
                    prepared.encoded = encode::encode(prepared.chunk);
                    if (done) {
                        done();
                    }
                    return prepared;
                }
                catch (...) {
                    // A column that failed must still wake its stream: a future nobody is
                    // woken for leaves the player waiting on a batch that never finishes.
                    if (done) {
                        done();
                    }
                    throw;
                }
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
