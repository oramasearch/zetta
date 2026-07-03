#pragma once

#include <cstddef>
#include <optional>

#include "zetta/add_nodes.hpp"            // AddNodesResult, add_nodes
#include "zetta/ir/execution_graph.hpp"  // ir::Node
#include "zetta/provisioner.hpp"         // Provisioner

// Sim-Python **cluster control bindings** (#522). The
// [Python frontend](python_frontend.hpp) lowers `cluster.add_nodes(n, provider=)`
// to an `act`-stage IR node (provisioning nodes is a side effect on the cluster),
// recording the call's arguments under the node's namespaced `python` provenance
// bag. This is the matching *execution* binding: take that lowered IR node and run
// it against the engine's permanent-add control action,
// [`zetta::add_nodes`](../add_nodes.hpp).
//
// The split — frontend lowers, this binding executes — keeps the frontend a pure,
// non-throwing parser (it never touches the engine) while the verb still runs
// end-to-end, the same shape as the ML verbs (`train`/`rank`/`explain`), which
// lower to `model`/`index`/`serve` steps the
// [`ChurnStepExecutor`](../churn_step_executor.hpp) already executes.
//
// **Cloud stays consent-gated.** The `provider=` argument names *which* backend the
// author intended (`azure`, `local`, …) and rides as provenance only — this binding
// does **not** construct a cloud provisioner from it. The `Provisioner&` is INJECTED
// by the host, which is the layer responsible for the per-action cloud consent gate
// (the hard cloud rule): the safe default touches no cloud. So a test drives a fake
// provisioner, and a real caller passes whatever provisioner it already holds.
//
// **No new JSON format.** The `python` provenance bag rides inside the IR node's
// free-form `attributes`, already validated by the IR JSON boundary
// ([`ir::from_json`](../ir/serialization.hpp)); the AddNodesRequest it drives has its
// own validated boundary. This binding parses no new format of its own.
//
// See docs/pipeline/python-frontend.md.
namespace zetta::input {

// Read the requested node count from a lowered sim-Python `cluster.add_nodes(...)`
// IR step. Reads the first positional non-negative integer argument
// (`cluster.add_nodes(3, …)`), else a `count` / `n` / `nodes` keyword argument
// (`cluster.add_nodes(n=10)`). Returns std::nullopt when no usable count is present,
// so a caller can tell "asked for 0" from "malformed control step".
std::optional<std::size_t> add_nodes_count(const ir::Node& step);

// Execute a lowered sim-Python `cluster.add_nodes(n, provider=)` IR step against the
// permanent-add control action (`zetta::add_nodes`) over the injected `provider`.
// Reads the count via `add_nodes_count`; a missing / zero count is a satisfied no-op
// (like `add_nodes(provider, 0)`). `provider` must outlive the call.
AddNodesResult run_add_nodes_step(const ir::Node& step, Provisioner& provider);

}  // namespace zetta::input
