#pragma once

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
// awaiting coroutine on its own executor. No caller-facing blocking, no
// condition variables in the API. (The internal std::mutex below is the
// contained cross-thread handoff of the queue itself — the cluster-outbox
// pattern sanctioned by the coroutines-first convention.)
namespace zetta::api {

class MainThreadBridge {
public:
    // Awaitable from any runtime coroutine (the Beast2 adapter): queue `work`
    // for the owning loop and resume once it has run. `work` must be quick
    // (it executes inside a tick) and must not throw. Compose a timeout at
    // the call site (the adapter maps a timed-out wait to a busy error).
    runtime::task<> run_on_owner(std::function<void()> work);

    // Called by the owning loop each tick/frame. Executes every queued work
    // item and signals its completion event.
    void drain();

    std::size_t pending() const;

private:
    struct PendingWork {
        std::function<void()> work;
        std::shared_ptr<runtime::async_event> completed;
    };

    // Contained cross-thread handoff (see header comment) — never exposed.
    mutable std::mutex queue_mutex_;
    std::deque<PendingWork> queue_;
};

}  // namespace zetta::api
