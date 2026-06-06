#pragma once

namespace zetta {

class Node;

// Run an already-started `node` in headless mode: tick it on a fixed cadence,
// logging liveness, until SIGINT/SIGTERM arrives, then return an exit code.
// Blocks the calling thread for the lifetime of the node. The caller owns the
// node's lifecycle and is responsible for stop()-ing it after this returns.
int run_headless(Node& node);

}  // namespace zetta
