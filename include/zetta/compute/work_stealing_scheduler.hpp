#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// The **local work-stealing scheduler** — the multi-core half of the native
// distributed accelerated-compute pass for UC-1 churn (ARC-10, #366). It is the
// "fluid, heterogeneity-tolerant" task pool the compute pass runs on: a job is
// split into many small tasks, every worker drains its own deque first, and an
// idle worker **steals** pending tasks from a busy one — so a worker that is
// faster (or simply less loaded) naturally processes more tasks, with no fixed
// per-worker capability model.
//
// This header is the local primitive only (one process, many cores). The
// cross-node half — idle/faster *nodes* stealing pending tasks from peers over
// the signal plane — is `zetta::compute::DistributedScheduler`
// (include/zetta/compute/distributed_scheduler.hpp), which is built on this and
// reuses it as its local execution engine. See docs/work-stealing-scheduler.md.
//
// Why a sibling to `zetta::runtime::thread_pool` rather than that pool directly:
// Capy's `thread_pool` (the seam's backend today) schedules off a single shared
// queue, which has no per-worker deque and so no *observable* steal to test or
// reason about. The compute pass wants genuine work-stealing — per-worker deques
// with an explicit, countable steal operation (the legibility this scheduler is
// built around). Task bodies may still enter the coroutine world via
// `zetta::runtime::block_on`; the scheduler itself stays agnostic to what a task
// does. See docs/runtime.md.
namespace zetta::compute {

// A unit of work: any nullary callable. Copyable (so it can sit in a deque and
// be moved between deques when stolen). For move-only captured state, wrap it in
// a shared_ptr inside the closure.
using Task = std::function<void()>;

// A snapshot of the scheduler's activity, for observability and tests. The
// canonical evidence that stealing happened: `steals > 0` and a lopsided
// `per_worker` distribution under an imbalanced load.
struct SchedulerStats {
    std::vector<std::size_t> per_worker;  // tasks executed by each worker
    std::size_t steals = 0;               // times a worker stole from another
    std::size_t executed = 0;             // total tasks executed (sum of above)
};

// A fixed pool of worker threads, each owning a deque of pending tasks, with
// work-stealing between them. Submit a task and it runs on some worker; when a
// worker's own deque empties it steals from another's, so load self-balances and
// faster/idle workers do more.
//
// The deque discipline is the standard Chase-Lev split, made legible with a
// per-deque mutex rather than a lock-free structure (correctness over cleverness;
// the tasks here are heavy compute shards, so per-deque locking is not the
// bottleneck): the owning worker pushes/pops its **back** (LIFO — best locality),
// a thief takes from a victim's **front** (FIFO — least contention with the
// owner). All public methods are thread-safe.
class WorkStealingScheduler {
public:
    // The default worker count: the hardware concurrency, at least 1.
    static unsigned default_worker_count();

    // Construct with `worker_count` worker threads (default: hardware
    // concurrency). **`worker_count == 0` is allowed and explicit** — it builds a
    // scheduler with no workers, a pure task *sink* that never runs anything
    // itself (the DistributedScheduler uses this for a node that only sources
    // work for peers to steal). With zero workers, `parallel_map` would block
    // forever — don't call it.
    explicit WorkStealingScheduler(unsigned worker_count = default_worker_count());

    // Requests stop, wakes every worker, and joins them. Tasks still queued (not
    // yet started) when the scheduler is destroyed are dropped — callers that
    // need a task's effect must ensure it has run first (e.g. via parallel_map,
    // which blocks until every task completes).
    ~WorkStealingScheduler();

    WorkStealingScheduler(const WorkStealingScheduler&) = delete;
    WorkStealingScheduler& operator=(const WorkStealingScheduler&) = delete;

    unsigned worker_count() const { return worker_count_; }

    // Submit one task. Called from a worker thread it lands on that worker's own
    // deque (LIFO locality); from any other thread it is round-robined across the
    // deques so all workers start busy. Returns immediately; the task runs later.
    void submit(Task task);

    // Run `function` over every input across the pool and return the results in
    // input order, blocking until all complete. This is the headline acceptance
    // case: an imbalanced pool (some workers slower or busier) still finishes
    // promptly because idle workers steal the slack, and the faster workers
    // process more of the inputs.
    //
    // MUST be called from a non-worker thread (it blocks the caller on a latch);
    // calling it from inside a task can deadlock the pool. `function` must not
    // throw across the scheduler boundary (zetta's no-exceptions-across-the-seam
    // convention).
    template <class Input, class Function>
    auto parallel_map(const std::vector<Input>& inputs, Function function)
        -> std::vector<std::decay_t<std::invoke_result_t<Function&, const Input&>>> {
        using Output =
            std::decay_t<std::invoke_result_t<Function&, const Input&>>;
        std::vector<Output> results(inputs.size());
        if (inputs.empty()) return results;

        // One latch counts every task down to zero; the caller waits on it. The
        // shared function is wrapped so each task writes its own slot — no
        // cross-task contention on `results`.
        std::latch done(static_cast<std::ptrdiff_t>(inputs.size()));
        auto shared_function =
            std::make_shared<Function>(std::move(function));
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            submit([this, index, &inputs, &results, &done, shared_function] {
                results[index] = (*shared_function)(inputs[index]);
                done.count_down();
            });
        }
        done.wait();
        return results;
    }

    // The index of the worker running the calling thread, or `worker_count()`
    // (an out-of-range sentinel) when called from a non-worker thread. Lets a
    // task body discover which worker it is on (e.g. to pin a compute device per
    // worker), and lets tests create a deliberately imbalanced pool.
    unsigned current_worker_index() const;

    // How many workers are currently running a task (0..worker_count). The
    // DistributedScheduler uses `active_count() < worker_count()` as its "I have
    // spare capacity to help a peer" signal.
    unsigned active_count() const {
        return active_.load(std::memory_order_relaxed);
    }

    // How many tasks are queued across all deques but not yet started.
    std::size_t queued() const {
        return pending_total_.load(std::memory_order_relaxed);
    }

    // A snapshot of per-worker counts + total steals (see SchedulerStats).
    SchedulerStats stats() const;

private:
    // One worker's deque + its lock. `std::deque` so the owner works the back and
    // a thief works the front without colliding.
    struct WorkerDeque {
        std::mutex mutex;
        std::deque<Task> tasks;
    };

    void worker_loop(unsigned index, std::stop_token stop_token);
    bool pop_own(unsigned index, Task& out);      // owner: take from the back
    bool steal_into(unsigned index, Task& out);   // thief: take a victim's front
    void wake_one();
    void wake_all();

    const unsigned worker_count_;
    // unique_ptr-boxed so the per-worker mutex/deque never moves (a vector of
    // mutexes can't be reallocated).
    std::vector<std::unique_ptr<WorkerDeque>> deques_;
    // Holds submissions when worker_count_ == 0 (a pure sink): nothing drains it,
    // but submit() stays safe and queued() stays honest.
    std::unique_ptr<WorkerDeque> orphan_;

    // Idle workers park here until work arrives or stop is requested. The
    // predicate is `pending_total_ > 0 || stop`; a short backstop timeout
    // recovers any missed wakeup without a hot spin.
    std::mutex idle_mutex_;
    std::condition_variable_any idle_cv_;
    std::atomic<std::size_t> pending_total_{0};
    std::atomic<unsigned> active_{0};

    // Round-robin cursor for submissions from non-worker threads.
    std::atomic<unsigned> submit_cursor_{0};

    // Per-worker executed counts (one writer each) + a shared steal counter.
    std::vector<std::atomic<std::size_t>> executed_;
    std::atomic<std::size_t> steals_{0};

    std::vector<std::jthread> workers_;
};

}  // namespace zetta::compute
