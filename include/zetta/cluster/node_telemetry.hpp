// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "zetta/cluster/node_metrics.hpp"
#include "zetta/cluster/signal.hpp"

// Cluster-wide live telemetry over the **signal plane** (#584, the engine half of
// the animated per-node gauges of #575 area 2). `system.node_metrics` measures
// only the host the query hits, so a node can't see its peers' utilization
// directly. This is the **ephemeral telemetry exchange** the node-metrics doc
// anticipated: each node periodically broadcasts its own freshly-sampled
// `NodeMetrics` to its peers as a fire-and-forget `node.metrics` event, and every
// node caches what it receives — so a single snapshot() on any node returns the
// whole cluster's live gauges (this node + every fresh peer).
//
// Like everything on the signal plane it is **deliberately NOT replicated** (a
// signal is a message, not durable state): telemetry is a measurement that goes
// stale in seconds, so the cache drops a peer whose last sample aged past a TTL,
// and nothing is gossiped through the CRDT. The payload rides a schema-validated
// JSON boundary (`node_metrics_wire_schema()`), the same discipline as the
// `CollectiveMessage` payload that shares this transport.
namespace zetta::cluster {

// One node's telemetry as carried over the wire / held in the cache: the sampled
// NodeMetrics plus the id of the node it describes. `age_seconds` + `is_local`
// are filled by NodeTelemetryExchange::snapshot() (not on the wire).
struct NodeTelemetrySample {
    std::string node_id;
    NodeMetrics metrics;
    double age_seconds = 0.0;  // seconds since this sample was received / recorded
    bool is_local = false;     // this node's own sample (recorded, not received)
};

// Encode a node's metrics to the wire JSON for a `node.metrics` signal-plane
// event. Only the **available** gauge groups are emitted — an unavailable counter
// is omitted, not zeroed, so the receiver reconstructs graceful degradation from
// field presence (a GPU-less host carries no gpu_* fields, never a misleading 0).
std::string encode_node_metrics(const NodeMetrics& metrics,
                                const std::string& node_id);

// Parse + validate a `node.metrics` payload back into a node id + metrics. The
// **boundary**: the payload is validated against the node-metrics wire schema
// (draft-07) before any field is read, so a malformed / hostile payload fails fast
// (reason written to `error` when non-null) instead of corrupting the cache.
// Non-throwing; nullopt on any failure. Mirrors `decode_collective_message`.
std::optional<NodeTelemetrySample> decode_node_metrics(
    std::string_view payload, std::string* error = nullptr);

// The wire schema for a `node.metrics` payload — the **code-owned source of
// truth**; `schemas/node_metrics.schema.json` is generated from it and
// drift-checked by `tests/test_schemas.cpp`.
const nlohmann::json& node_metrics_wire_schema();

// The most buckets one `node.metrics` payload lists by name (zetta#1259). A node with more
// sends the largest ones and counts the rest in `buckets_omitted`; their bytes stay in
// `kinds.buckets` and `total_bytes`, so the cluster's totals never lose them.
inline constexpr std::size_t kMaxWireCensusBuckets = 256;

// One member's census in a cluster answer (zetta#1259).
struct MemberStorageCensus {
    std::string node_id;
    bool is_local = false;
    zetta::data::StorageCensus census;
    std::string measured_at;     // ISO-8601 UTC, by the measuring node's clock
    double age_seconds = 0.0;    // how old the census is now, by this node's clock
    std::uint64_t buckets_omitted = 0;
};

// A member that is not in the cluster's total, and why: `not_reporting` (no telemetry from it
// within the freshness window: it is down, partitioned, or older than the census), `not_measured`
// (it reports telemetry but has not measured its disk yet) or `stale` (its census is older than
// the stale limit).
struct MissingStorageCensus {
    std::string node_id;
    std::string reason;
};

// A whole cluster's disk by kind: every member with a fresh census, summed, and the members
// without one named, never counted as zero. Bytes are physical: a replicated dataset or bucket
// counts once on every member that holds a copy.
struct ClusterStorageCensus {
    zetta::data::StorageCensus total;  // kinds, buckets by name and total_bytes, summed
    std::vector<MemberStorageCensus> nodes;
    std::vector<MissingStorageCensus> missing;
};

// Folds the telemetry samples into a cluster census. `members` is the cluster membership (every
// node id the replicated node-list knows); a sampled node outside it still counts. A census whose
// age (its age when sampled plus the sample's age) is at least `stale_after` is `stale`. Nodes and
// missing members are node-id ascending.
ClusterStorageCensus fold_storage_census(const std::vector<std::string>& members,
                                         const std::vector<NodeTelemetrySample>& samples,
                                         std::chrono::duration<double> stale_after);

// The cluster-wide telemetry cache fed by the signal plane. It is a SignalHandler
// wired as the JobCoordinator's **fallback** (so it coexists with the steal /
// collective signals the coordinator already routes): it caches each peer's
// `node.metrics` event keyed by the transport-verified sender id (`Signal::from`),
// and the local node records its own sample here too — so one snapshot() returns
// the whole cluster's live gauges. Stale peers (no sample within the snapshot TTL)
// drop out, so a departed node's gauges don't dangle.
//
// **Thread-safe**: on_event runs on the cluster's network thread; record_local +
// snapshot run on the loop thread. An internal mutex guards the cache.
class NodeTelemetryExchange : public SignalHandler {
public:
    // The signal-plane event verb this exchange broadcasts + handles.
    static constexpr std::string_view kMethod = "node.metrics";

    // Record this node's own freshly-sampled metrics (called from Node::tick),
    // so the local node appears in snapshot() alongside its peers.
    void record_local(const std::string& node_id, const NodeMetrics& metrics);

    // SignalHandler: cache a peer's `node.metrics` event (validated at the
    // boundary); a malformed payload or any other event is ignored. on_request /
    // on_task keep the SignalHandler defaults (this only consumes events).
    void on_event(const Signal& event) override;

    // This node + every peer whose last sample is within `ttl`, each tagged with
    // its age and whether it is the local node. Node-id ascending (deterministic).
    //
    // The freshness window is **half-open**: an entry is kept while `age < ttl` and
    // dropped once `age >= ttl`. So `ttl == 0` keeps nothing, whatever the clock did
    // between the record and the snapshot (#834).
    std::vector<NodeTelemetrySample> snapshot(
        std::chrono::duration<double> ttl = std::chrono::seconds(8)) const;

private:
    struct Entry {
        NodeMetrics metrics;
        std::chrono::steady_clock::time_point received{};
        bool is_local = false;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;  // node_id → latest sample
};

}  // namespace zetta::cluster