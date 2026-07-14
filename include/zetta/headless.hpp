#pragma once

#include <chrono>
#include <functional>

namespace zetta {

class Node;
namespace cluster { class Cluster; }
// Run an already-started `node` in headless mode: tick it on a fixed cadence,
// logging liveness (and the cluster role / peer count), until SIGINT/SIGTERM
// arrives, then return an exit code. Blocks the calling thread for the lifetime
// of the node. The caller owns the node + cluster lifecycle and stop()s them
// after this returns.
// `on_tick` (optional) runs each iteration to drain the API queue — the
// owning-loop hook the embedded API server's bridge drains through.
// `wait_for_work` (optional) is the loop's idle wait: it blocks up to the tick
// interval but returns the instant a request is enqueued, so the drain runs
// immediately instead of on the next 10 Hz tick (#731). When absent the loop
// falls back to an unconditional sleep (no API server wired).
int run_headless(Node& node, const zetta::cluster::Cluster& cluster,
                 const std::function<void()>& on_tick = {},
                 const std::function<void(std::chrono::milliseconds)>& wait_for_work = {});

}  // namespace zetta
