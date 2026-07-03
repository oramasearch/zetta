#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

// Ownership for in-flight background actions started by `kind: job`
// capabilities (#617). The durable async protocol is the engine's own handles
// — a Run (`run:<uuid>`) or ComputeJob, polled through the read surface
// (docs/api/registry-design.md → "no new job infrastructure") — but the
// in-process worker object driving the work (an AsyncChurnRun, a testkit
// AsyncCaseRun) must stay alive until it finishes. In the headed app that
// owner is App (one member per action); on the API surface it is this class:
// a job handler adopts the handle, and the loop that owns the Node prunes
// finished ones each drain, so destructors (which may join a worker) always
// run on the owner thread.
namespace zetta::api {

class BackgroundActions {
public:
    // Keep `handle` alive until `done()` first returns true. `done` is called
    // only from prune() (the owner thread) and must be non-throwing. Call on
    // the owner thread. A null handle or done is ignored.
    void adopt(std::shared_ptr<void> handle, std::function<bool()> done);

    // Drop every adopted handle whose done() is true — their destructors run
    // here, on the calling (owner) thread. The owning loop calls this each
    // tick/drain, beside the bridge drain.
    void prune();

    std::size_t pending() const { return entries_.size(); }

private:
    struct Entry {
        std::shared_ptr<void> handle;
        std::function<bool()> done;
    };
    std::vector<Entry> entries_;
};

}  // namespace zetta::api
