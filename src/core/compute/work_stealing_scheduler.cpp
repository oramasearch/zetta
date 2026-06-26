#include "zetta/compute/work_stealing_scheduler.hpp"

#include <chrono>

namespace zetta::compute {

namespace {

// Identifies the worker (if any) running the calling thread. A thread that is a
// worker of scheduler S has `scheduler == S` and `index` in [0, worker_count);
// every other thread (the submitter, a test, a worker of a *different*
// scheduler) sees `scheduler == nullptr`. Thread-local so the scheduler can
// route a self-submission onto the running worker's own deque and answer
// current_worker_index() without a lookup.
struct WorkerIdentity {
    const WorkStealingScheduler* scheduler = nullptr;
    unsigned index = 0;
};
thread_local WorkerIdentity g_worker_identity;

// A short backstop on the idle wait: even if a wakeup notification is lost
// (submit raced the worker into the wait), the worker re-checks within this and
// picks the work up — no hot spin, bounded latency.
constexpr std::chrono::milliseconds kIdleBackstop{5};

}  // namespace

unsigned WorkStealingScheduler::default_worker_count() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 1u : hardware;
}

WorkStealingScheduler::WorkStealingScheduler(unsigned worker_count)
    : worker_count_(worker_count), executed_(worker_count) {
    if (worker_count_ == 0) orphan_ = std::make_unique<WorkerDeque>();
    deques_.reserve(worker_count_);
    for (unsigned index = 0; index < worker_count_; ++index) {
        deques_.push_back(std::make_unique<WorkerDeque>());
    }
    workers_.reserve(worker_count_);
    for (unsigned index = 0; index < worker_count_; ++index) {
        workers_.emplace_back(
            [this, index](std::stop_token stop_token) {
                worker_loop(index, std::move(stop_token));
            });
    }
}

WorkStealingScheduler::~WorkStealingScheduler() {
    // Ask every worker to stop, then wake them so a parked worker observes it.
    // std::jthread's destructor requests stop + joins, but we wake explicitly so
    // the join doesn't wait out the idle backstop.
    for (std::jthread& worker : workers_) worker.request_stop();
    wake_all();
    // jthread destructors join here.
}

void WorkStealingScheduler::submit(Task task) {
    // With no workers the scheduler is a pure sink: the task is held (so queued()
    // stays honest) but never runs, and there is nobody to wake.
    if (worker_count_ == 0) {
        const std::lock_guard<std::mutex> lock(orphan_->mutex);
        orphan_->tasks.push_back(std::move(task));
        pending_total_.fetch_add(1, std::memory_order_release);
        return;
    }
    // A self-submission (from one of this scheduler's workers) goes on that
    // worker's own deque back for LIFO locality; anything else is round-robined
    // so a burst spreads across all workers.
    unsigned target = 0;
    if (g_worker_identity.scheduler == this) {
        target = g_worker_identity.index;
    } else {
        target = submit_cursor_.fetch_add(1, std::memory_order_relaxed) %
                 worker_count_;
    }
    {
        const std::lock_guard<std::mutex> lock(deques_[target]->mutex);
        deques_[target]->tasks.push_back(std::move(task));
    }
    pending_total_.fetch_add(1, std::memory_order_release);
    wake_one();
}

bool WorkStealingScheduler::pop_own(unsigned index, Task& out) {
    const std::lock_guard<std::mutex> lock(deques_[index]->mutex);
    if (deques_[index]->tasks.empty()) return false;
    out = std::move(deques_[index]->tasks.back());
    deques_[index]->tasks.pop_back();
    return true;
}

bool WorkStealingScheduler::steal_into(unsigned index, Task& out) {
    // Scan the other deques round-robin from the next index, taking from a
    // victim's front (FIFO) to stay clear of the owner working its back.
    for (unsigned offset = 1; offset < worker_count_; ++offset) {
        const unsigned victim = (index + offset) % worker_count_;
        const std::lock_guard<std::mutex> lock(deques_[victim]->mutex);
        if (deques_[victim]->tasks.empty()) continue;
        out = std::move(deques_[victim]->tasks.front());
        deques_[victim]->tasks.pop_front();
        steals_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void WorkStealingScheduler::worker_loop(unsigned index,
                                        std::stop_token stop_token) {
    g_worker_identity = WorkerIdentity{.scheduler = this, .index = index};

    while (!stop_token.stop_requested()) {
        Task task;
        if (pop_own(index, task) || steal_into(index, task)) {
            pending_total_.fetch_sub(1, std::memory_order_acq_rel);
            active_.fetch_add(1, std::memory_order_relaxed);
            task();
            active_.fetch_sub(1, std::memory_order_relaxed);
            executed_[index].fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // Nothing to run: park until work arrives or stop is requested. The
        // backstop bounds any lost-wakeup latency without busy-waiting.
        std::unique_lock<std::mutex> lock(idle_mutex_);
        idle_cv_.wait_for(lock, stop_token, kIdleBackstop, [this] {
            return pending_total_.load(std::memory_order_acquire) > 0;
        });
    }

    g_worker_identity = WorkerIdentity{};
}

void WorkStealingScheduler::wake_one() {
    idle_cv_.notify_one();
}

void WorkStealingScheduler::wake_all() {
    idle_cv_.notify_all();
}

unsigned WorkStealingScheduler::current_worker_index() const {
    if (g_worker_identity.scheduler == this) return g_worker_identity.index;
    return worker_count_;  // out-of-range sentinel: not one of our workers
}

SchedulerStats WorkStealingScheduler::stats() const {
    SchedulerStats snapshot;
    snapshot.per_worker.reserve(worker_count_);
    std::size_t total = 0;
    for (const std::atomic<std::size_t>& count : executed_) {
        const std::size_t value = count.load(std::memory_order_relaxed);
        snapshot.per_worker.push_back(value);
        total += value;
    }
    snapshot.executed = total;
    snapshot.steals = steals_.load(std::memory_order_relaxed);
    return snapshot;
}

}  // namespace zetta::compute
