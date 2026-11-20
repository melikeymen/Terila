#pragma once

// A small coroutine type for the places where the server has to do a long job in pieces:
// building an answer that has to wait for the socket to drain, walking a list of chunks
// while the write queue is full, or chaining steps that each may need to pause.
//
// A task does not start by itself. Whoever owns it resumes it, one step at a time, and the
// task suspends whenever it has to wait. That keeps the server single threaded: no locks,
// no threads, and no coroutine running behind the owner's back.

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace terila::async {

    namespace detail {

        // Where the innermost coroutine that just suspended itself reports to. A task that
        // waits on a child has to remember the child, otherwise resuming it would call
        // await_resume on a value that is not there yet.
        struct Context {
            std::coroutine_handle<> suspended = nullptr;
        };

        inline thread_local Context* active = nullptr;

        // The result slot doubles as the promise's return hook: a task of void has no value
        // to store and must not declare return_value at all.
        template<class T>
        struct ResultSlot {
            std::optional<T> value;
            void return_value(T item) { value.emplace(std::move(item)); }
            T take() { return std::move(*value); }
        };

        template<>
        struct ResultSlot<void> {
            void return_void() noexcept {}
            void take() noexcept {}
        };

    } // namespace detail

    template<class T = void>
    class Task {
    public:
        struct promise_type : detail::ResultSlot<T> {
            std::exception_ptr failure;
            std::coroutine_handle<> continuation = nullptr;

            // The owner decides when work starts, and the owner is the only one who resumes.
            std::suspend_always initial_suspend() noexcept { return {}; }

            // Hand control back to whoever was waiting on this task instead of returning to
            // a scheduler: the chain unrolls without a queue in between.
            struct FinishAwaiter {
                bool await_ready() const noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> self) const noexcept {
                    const auto next = self.promise().continuation;
                    return next != nullptr ? next : std::noop_coroutine();
                }
                void await_resume() const noexcept {}
            };

            FinishAwaiter final_suspend() noexcept { return {}; }
            void unhandled_exception() { failure = std::current_exception(); }

            Task get_return_object() {
                return Task(std::coroutine_handle<promise_type>::from_promise(*this));
            }


        };

        Task() = default;
        explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;
        Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)),
            suspended_(std::exchange(other.suspended_, nullptr)) {}
        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                destroy();
                handle_ = std::exchange(other.handle_, nullptr);
                suspended_ = std::exchange(other.suspended_, nullptr);
            }
            return *this;
        }

        ~Task() { destroy(); }

        // One step. False means the task has finished (or there was nothing to run).
        bool step() {
            if (handle_ == nullptr || handle_.done()) {
                return false;
            }

            // Resume the coroutine that suspended last, not the one this Task owns. After a
            // child yields, the parent is still parked on the child's await, so resuming the
            // parent would read a result the child has not produced yet.
            std::coroutine_handle<> current = handle_;
            if (suspended_ != nullptr && !suspended_.done()) {
                current = suspended_;
            }

            detail::Context context;
            auto* previous = detail::active;
            detail::active = &context;
            current.resume();
            detail::active = previous;
            suspended_ = context.suspended;
            return !handle_.done();
        }

        bool finished() const noexcept {
            return handle_ == nullptr || handle_.done();
        }

        // The task failed: rethrow what the body threw.
        void rethrow_if_failed() {
            if (handle_ != nullptr && handle_.promise().failure != nullptr) {
                std::rethrow_exception(handle_.promise().failure);
            }
        }

        template<class U = T>
            requires(!std::is_void_v<U>)
        U take() {
            rethrow_if_failed();
            U value = handle_.promise().take();
            destroy();
            return value;
        }

        template<class U = T>
            requires(std::is_void_v<U>)
        void take() {
            rethrow_if_failed();
            destroy();
        }

        // Run the task to the end on this thread: for the places that only need the result
        // and are already off the tick.
        template<class U = T>
        U wait() {
            std::coroutine_handle<> current = handle_;
            while (!finished()) {
                detail::Context context;
                auto* previous = detail::active;
                detail::active = &context;
                current.resume();
                detail::active = previous;
                if (context.suspended != nullptr) {
                    current = context.suspended;
                }
            }
            if constexpr (std::is_void_v<U>) {
                rethrow_if_failed();
                destroy();
            }
            else {
                return take();
            }
        }

        // co_await other_task: the child runs first, the parent continues when it finishes.
        bool await_ready() const noexcept { return finished(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
            handle_.promise().continuation = caller;
            return handle_;
        }

        T await_resume() {
            rethrow_if_failed();
            if constexpr (std::is_void_v<T>) {
                return;
            }
            else {
                return handle_.promise().take();
            }
        }

        std::coroutine_handle<promise_type> handle() const noexcept { return handle_; }

    private:
        std::coroutine_handle<promise_type> handle_ = nullptr;
        // The innermost coroutine that suspended inside this task's chain.
        std::coroutine_handle<> suspended_ = nullptr;

        void destroy() {
            if (handle_ != nullptr) {
                handle_.destroy();
                handle_ = nullptr;
            }
            suspended_ = nullptr;
        }
    };

    // Suspends a task until its owner resumes it: the "rest of this frame" point. The
    // coroutine that yields is what the owner has to resume next, so it reports itself.
    struct Yield {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> self) const noexcept {
            if (detail::active != nullptr) {
                detail::active->suspended = self;
            }
        }
        void await_resume() const noexcept {}
    };

    // Runs a Task<void> step by step. The owner keeps this next to the state the task reads
    // and calls resume() whenever that state may have changed.
    class Stream {
    public:
        Stream() = default;
        explicit Stream(Task<void> task) : task_(std::move(task)) {}

        // One step forward: run until the task yields again or ends. True while it still
        // has work left.
        bool resume() {
            if (task_.finished()) {
                return false;
            }
            if (current_ == nullptr || current_.done()) {
                current_ = task_.handle();
            }

            detail::Context context;
            auto* previous = detail::active;
            detail::active = &context;
            current_.resume();
            detail::active = previous;
            if (context.suspended != nullptr) {
                current_ = context.suspended;
            }
            return !task_.finished();
        }

        bool finished() const noexcept { return task_.finished(); }

        void restart(Task<void> task) {
            task_ = std::move(task);
            current_ = nullptr;
        }

    private:
        Task<void> task_;
        // The coroutine to resume next, which is the innermost one that suspended.
        std::coroutine_handle<> current_ = nullptr;
    };

} // namespace terila::async
