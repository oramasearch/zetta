#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "zetta/runtime.hpp"

// The main-thread bridge — the key threading decision of the API surface
// (docs/api/registry-design.md). The Node tree is owned by one loop (the
// headless tick loop, or the headed frame loop); the HTTP server's coroutines
// run elsewhere and must NEVER touch the Node directly.
//
// Coroutine-first: a server coroutine `co_await`s run_on_owner(work); the
// owning loop drains the queue right where Node::tick() already runs, executes
// the work, and sets the task's runtime::async_event — which resumes the
// awaiting coroutine on its own executor. No caller-facing blocking. (The
// internal std::mutex below is the contained cross-thread handoff of the queue
// itself — the cluster-outbox pattern sanctioned by the coroutines-first
// convention.)
//
// Wake-on-enqueue (#731): enqueuing also WAKES the owning loop so it drains
// *when work arrives* instead of polling at the 10 Hz tick — the ~100 ms/request
// floor was the whole latency of waiting for the next timed drain. The headless
// loop waits on `wait_for_work()` (a condition variable, released immediately on
// enqueue); the headed frame loop registers `set_wakeup()` (an SDL event push)
// so its `SDL_WaitEventTimeout` returns early. The single-owner-thread model
// (#624) is unchanged — this only removes the fixed poll delay.
namespace zetta::api {

class MainThreadBridge {
public:
    // Awaitable from any runtime coroutine (the Beast2 adapter): queue `work`
    // for the owning loop and resume once it has run. `work` must be quick
    // (it executes inside a tick) and must not throw — a throw is contained
    // by drain() (logged; the awaiting coroutine still resumes, #1007), but
    // whatever the work was meant to produce is lost. Compose a timeout at
    // the call site (the adapter maps a timed-out wait to a busy error).
    runtime::task<> run_on_owner(std::function<void()> work);

    // Called by the owning loop each tick/frame. Executes every queued work
    // item and signals its completion event.
    void drain();

    // The owning loop's idle wait (headless): blocks up to `max_idle` but
    // returns the moment work is enqueued, so a drain follows immediately. The
    // timeout stays as the max idle interval for background bookkeeping.
    void wait_for_work(std::chrono::milliseconds max_idle);

    // Register a wakeup the owning loop provides (headed: push an SDL event so
    // its event wait returns). Called once at startup; run_on_owner invokes it
    // after enqueue. Optional — headless uses wait_for_work instead.
    void set_wakeup(std::function<void()> wakeup);

    std::size_t pending() const;

private:
    struct PendingWork {
        std::function<void()> work;
        std::shared_ptr<runtime::async_event> completed;
    };

    // Contained cross-thread handoff (see header comment) — never exposed.
    mutable std::mutex queue_mutex_;
    std::condition_variable work_ready_;  // wakes the owning loop's idle wait
    std::deque<PendingWork> queue_;
    std::function<void()> wakeup_;  // owner-loop wake for non-cv loops (headed)
};

}  // namespace zetta::api
