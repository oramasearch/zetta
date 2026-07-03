#pragma once

#include <functional>

namespace zetta {

class Node;
namespace cluster { class Cluster; }
// Run an already-started `node` in headless mode: tick it on a fixed cadence,
// logging liveness (and the cluster role / peer count), until SIGINT/SIGTERM
// arrives, then return an exit code. Blocks the calling thread for the lifetime
// of the node. The caller owns the node + cluster lifecycle and stop()s them
// after this returns.
// `on_tick` (optional) runs each heartbeat right after Node::tick() — the
// owning-loop hook the embedded API server's bridge drains through.
int run_headless(Node& node, const zetta::cluster::Cluster& cluster,
                 const std::function<void()>& on_tick = {});

}  // namespace zetta
