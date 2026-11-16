// SPDX-License-Identifier: MIT

#pragma once

// GUI-internal animation state for the distributed-algorithm-at-work overlay on
// the cluster node-box canvas (#590, epic #575 area 5). It is the animation layer
// the issue asks for, and — like the per-node gauges (#584) — a **thin renderer**
// over the engine's live instrumentation: it owns NO numbers of its own, only the
// short-lived eased "flows" needed to ANIMATE what the read views report.
//
// The data sources are the three live, ephemeral read views the prior slices
// landed (no CRDT, no invented numbers — see docs/compute/pipeline-instrumentation.md):
//   - system.dataset_partitions  (Node::dataset_partitions())  → placement + rebalance
//   - system.pipeline_jobs        (Node::pipeline_jobs())        → near-data vs wire bytes
//   - system.collective_rounds    (coordinator->collectives())  → scatter/gather rounds
// Each frame draw_cluster() snapshots those into a PipelineInputs, the animator
// DIFFS it against the last snapshot, and a change spawns a brief PipelineFlow that
// glides over the node boxes for ~1s and then expires. Steady state is a couple of
// cheap map lookups + a bounded flow list, so the overlay never starves a live
// resize (the render_frame/SDL_WaitEventTimeout cadence constraint).
//
// Pure data — no ImGui/SDL here, so app.hpp may include it without pulling the GUI
// toolkit into every translation unit. The diff (update_cluster_pipeline) lives in
// src/gui/app_cluster.cpp; the drawing (draw_pipeline_flow + the flow palette) lives
// in src/gui/app_operations_internal.hpp, shared with the Search view's cluster
// strip (#643), which replays a search's per-node provenance through these flows.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace zetta::gui {

// What a single animated flow depicts. The vocabulary maps directly onto the
// issue's list (partitioning, near-data load, scatter/gather, train rounds,
// rebalance, wire-vs-local). Each is born from a real instrumentation change.
enum class PipelineFlowKind {
    PartitionPlaced,  // rows → partition → owner node (a new system.dataset_partitions row)
    PartitionMoved,   // rebalance-on-join: a partition slid from an old owner onto a new one
    NearDataRead,     // a near-data job read its local shard (bytes stay home, never moved)
    CollectiveScatter,  // a collective round: root → each participant (broadcast leg)
    CollectiveGather,   // a collective round: each participant → root (gather leg)
    WireMoved,        // a job's partials left a node over the wire (wire_bytes_moved)
    LocalCompute,     // a round on a single-node cluster (no peer to flow to) — a self pulse
};

// One in-flight animated flow. `from`/`to` are node ids resolved to box centers by
// the renderer each frame (so the animation tracks a live resize / reflow); a flow
// whose endpoint has left the cluster is simply skipped. `age` starts at `-delay`
// (a flow can be scheduled to begin slightly later, e.g. scatter after gather) and
// the flow expires once `age >= lifetime`. `weight` (0..1) scales the dot/ring so a
// big transfer reads heavier than a small one; `label` is an optional short mono
// caption drawn briefly at the target (row count / byte rate / op name).
struct PipelineFlow {
    PipelineFlowKind kind = PipelineFlowKind::PartitionPlaced;
    std::string from;       // source node id ("" for a from-data drop)
    std::string to;         // destination / target node id
    float age = 0.0f;       // seconds since scheduled (negative = not yet started)
    float lifetime = 1.0f;  // seconds the flow lives once started
    float weight = 1.0f;    // 0..1 relative magnitude (thickness)
    std::string label;      // short caption, drawn briefly near the target
};

// The animator: the last-seen instrumentation baseline to diff against, the
// bounded list of in-flight flows, and a per-node "activity" level (eased) used to
// glow a node box while it is doing work. Plus the live counters the overlay strip
// echoes verbatim from the read views (so the animation is anchored to real
// numbers, never invented). All bounded — `seen_jobs` / `partition_owners` are
// pruned to the current snapshot each update, and `flows` is capped — so a
// long-lived session can't grow it without limit.
struct ClusterPipelineState {
    // The most in-flight flows kept at once (older ones are dropped when full), and
    // the most NEW flows spawned per update (so a burst — e.g. returning to the view
    // after a long run — folds into the baseline instead of replaying as a storm).
    static constexpr std::size_t kMaxFlows = 64;
    static constexpr int kMaxSpawnPerUpdate = 14;

    bool primed = false;  // first update records the baseline WITHOUT spawning flows

    // Diff anchors (the last values we reacted to).
    std::uint64_t last_rounds = 0;       // collective rounds_completed
    std::uint64_t last_wire_bytes = 0;   // cumulative collective wire bytes
    // Per-partition last-seen owner set, to detect a placement (new id) vs a move
    // (owners changed). Pruned to the live catalog each update.
    std::unordered_map<std::string, std::vector<std::string>> partition_owners;
    // Job ids already animated, pruned to the bounded ring each update.
    std::unordered_map<std::string, bool> seen_jobs;

    // In-flight flows + a paced cooldown so fast-ticking collective rounds pulse at
    // a legible ~2 Hz rather than flooding the canvas (the strip still shows the
    // true round rate). `peak_weight` is a decaying reference the wire/near-data
    // weights normalise against so an idle link doesn't paint a heavy flow.
    std::vector<PipelineFlow> flows;
    float collective_cooldown = 0.0f;
    double peak_bytes = 1.0;

    // Eased "this node is working" level per node id (0..1), bumped when a flow
    // touches the node and decayed each frame. Drawn as a soft ring on the box.
    std::unordered_map<std::string, float> node_activity;

    // Cumulative-since-priming tallies for the overlay strip (the animation's own
    // count of events it witnessed — placements / moves — which the read views don't
    // pre-aggregate). The live byte/round totals come straight from the inputs.
    std::uint64_t placements_seen = 0;
    std::uint64_t moves_seen = 0;
};

// One partition from the catalog, reduced to what the animator diffs (id + owners +
// row count). Built in draw_cluster from Node::dataset_partitions() so this header
// stays free of zetta-core types.
struct PipelinePartitionRef {
    std::string id;
    std::vector<std::string> owners;
    std::uint64_t rows = 0;
};

// One recorded near-data job, reduced to what the animator diffs.
struct PipelineJobRef {
    std::string id;
    std::string node;       // the node that ran + recorded it
    std::string kind;       // "group_by" / "featurize" / "train"
    std::uint64_t local_bytes = 0;
    std::uint64_t wire_bytes = 0;
    std::uint64_t rows = 0;
    bool succeeded = false;
};

// The per-frame instrumentation snapshot the animator diffs. A plain value type so
// the read happens ONCE in draw_cluster and the pure-data header carries no engine
// dependency. `participants` is the live collective participant list (sorted, so
// front() is the leaderless root); `nodes` is the current box membership.
struct PipelineInputs {
    bool attached = false;             // a JobCoordinator is attached (a live run can occur)
    std::uint64_t rounds_completed = 0;
    std::uint64_t wire_bytes = 0;
    std::vector<std::string> participants;  // collective participants (sorted; front = root)
    std::vector<std::string> nodes;         // live cluster member ids (the boxes)
    std::vector<PipelinePartitionRef> partitions;
    std::vector<PipelineJobRef> jobs;
};

}  // namespace zetta::gui
