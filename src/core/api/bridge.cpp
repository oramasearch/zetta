// SPDX-License-Identifier: MIT

#include "zetta/api/bridge.hpp"

#include <cstdio>
#include <exception>
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
    // Queued work can borrow the awaiting request's frame. Transport shutdown
    // must not cancel this completion wait and destroy that frame before the
    // owner runs the closure (#1077). Keep the executor, but mask cancellation
    // until drain() has run the work and signaled completion.
    [[maybe_unused]] auto ready = co_await runtime::with_stop_token(
        completed->wait(), std::stop_token{});
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
        // `work` must not throw (see the header), but if it does, the awaiting
        // coroutine must still resume: an unset event parks the request forever
        // (the client sees no response, #1007), and the exception would also
        // skip every later item in this batch and unwind the owner loop. The
        // awaiting side reads whatever the work left behind — for a capability
        // call, a default (failed) result, which it answers as a 500.
        try {
            pending_work.work();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "zetta: owner-loop work threw: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "zetta: owner-loop work threw an unknown error\n");
        }
        pending_work.completed->set();
    }
}

void MainThreadBridge::wait_for_work(std::chrono::milliseconds max_idle) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    // Returns immediately if work is already queued, wakes the moment
    // run_on_owner enqueues, and otherwise caps the idle wait at max_idle
    // (the background-bookkeeping heartbeat) — the #731 fix.
    work_ready_.wait_for(lock, max_idle, [this] { return !queue_.empty() || poked_; });
    poked_ = false;
}

void MainThreadBridge::poke() {
    std::function<void()> wakeup;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        poked_ = true;
        wakeup = wakeup_;
    }
    work_ready_.notify_one();
    if (wakeup) wakeup();
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
