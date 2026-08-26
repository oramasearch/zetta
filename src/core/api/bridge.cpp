#include "zetta/api/bridge.hpp"

#include <utility>
#include <vector>

namespace zetta::api {

runtime::task<> MainThreadBridge::run_on_owner(std::function<void()> work) {
    auto completed = std::make_shared<runtime::async_event>();
    std::function<void()> wakeup;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(PendingWork{std::move(work), completed});
        wakeup = wakeup_;
    }
    // Wake the owning loop so it drains now, not on the next 10 Hz tick (#731):
    // the cv releases a headless wait_for_work; the callback (if set) wakes a
    // headed frame loop's event wait.
    work_ready_.notify_one();
    if (wakeup) wakeup();
    // The owner ran the work; the wait's io_result carries only whether the
    // event was cancelled, which cannot happen here (the event is set by
    // drain() before this resumes). Bound, like runtime::offload().
    [[maybe_unused]] auto ready = co_await completed->wait();
}

void MainThreadBridge::drain() {
    std::vector<PendingWork> batch;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        batch.assign(std::make_move_iterator(queue_.begin()),
                     std::make_move_iterator(queue_.end()));
        queue_.clear();
    }
    for (PendingWork& pending_work : batch) {
        pending_work.work();
        pending_work.completed->set();
    }
}

void MainThreadBridge::wait_for_work(std::chrono::milliseconds max_idle) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    // Returns immediately if work is already queued, wakes the moment
    // run_on_owner enqueues, and otherwise caps the idle wait at max_idle
    // (the background-bookkeeping heartbeat) — the #731 fix.
    work_ready_.wait_for(lock, max_idle, [this] { return !queue_.empty(); });
}

void MainThreadBridge::set_wakeup(std::function<void()> wakeup) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    wakeup_ = std::move(wakeup);
}

std::size_t MainThreadBridge::pending() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
}

}  // namespace zetta::api
