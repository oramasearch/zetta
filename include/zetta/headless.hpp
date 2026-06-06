#pragma once

namespace zetta {

class Node;
class Cluster;

// Run an already-started `node` in headless mode: tick it on a fixed cadence,
// logging liveness (and the cluster role / peer count), until SIGINT/SIGTERM
// arrives, then return an exit code. Blocks the calling thread for the lifetime
// of the node. The caller owns the node + cluster lifecycle and stop()s them
// after this returns.
int run_headless(Node& node, const Cluster& cluster);

}  // namespace zetta
