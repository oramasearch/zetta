// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/connect/connector.hpp"
#include "zetta/data/partition_scheduler.hpp"

namespace zetta { class Node; }
namespace zetta::compute { class CoordinatedLaunchListener; }
namespace zetta::data {
class Database;
class DatasetPartition;
class Table;
}  // namespace zetta::data

// **The cross-node connector ingest** — one query's result pulled with the
// cluster's aggregate bandwidth and landed where it was pulled
// ([#833](https://github.com/oramasearch/zetta/issues/833) section 7,
// [#829](https://github.com/oramasearch/zetta/issues/829)).
//
// Tiling a result set and then running every tile on the coordinating node is a
// parallel loop, not a distributed algorithm: it spends one node's uplink and
// N-1 nodes' worth of nothing, and the rows cross the wire twice (once to the
// coordinator, once to their eventual owner). This is the other half:
//
//   1. **SUBMIT** — one node POSTs the statement and reads back the handle plus
//      `partitionInfo[]`: the per-chunk row counts (`submit_connector_statement`).
//   2. **LAUNCH** — it carries (handle, roster, row counts) to every participant
//      as a `run.launch` task over the signal plane
//      ([coordinated launch](../compute/coordinated_launch.hpp)) — the existing
//      co-launch machinery, not a second synchronization path. **No credential
//      travels**: the launch names a *saved connection*, and saved connections
//      are replicate-to-all precisely so each node presents the token itself.
//   3. **FETCH** — the launch carries the **partition plan** the coordinator's
//      [`PartitionScheduler`](../data/partition_scheduler.hpp) computed. Every
//      participant tiles the result with `plan_fetch_assignments(roster, row
//      counts, the plan's per-node shares)` and pulls **only its own tile**,
//      several requests in flight (`fetch_result_partitions`, bounded).
//
//      The tiling used to be derived independently on each node — a pure
//      function of (roster, row counts), so nothing travelled. Capacity-aware
//      placement reads live telemetry, which differs per node by construction,
//      so the plan is now **computed once by the coordinator and transmitted**.
//      Participants use what they received; nobody recomputes.
//
//      A Snowflake result set is a **flexible** source (its chunks are freely
//      assignable), so the fetch lands directly in the target shape and there is
//      **no rebalance** afterwards — see docs/data/partition-scheduler.md.
//   4. **LAND** — each node writes its slice as
//      [partition-only](../data/partition_only) partitions **it** owns
//      ([fetch affinity](../data/fetch_affinity_placement.hpp)). Rows never
//      enter the CRDT, so nothing re-replicates to the coordinator: the data
//      crosses the wire exactly once.
//
// **All or nothing.** A node dying mid-fetch **fails the whole ingest**: the
// participants agree through one `allreduce` over the launch id, and a
// contribution that is missing, late, or unsuccessful fails it for everyone.
// Each node then discards the partitions it wrote and the initiator drops the
// table, so the dataset either exists complete or does not exist. It is a
// *cache* — re-running the ingest is the expected remedy, and reassigning an
// orphaned tile would mean tracking assignments rather than deriving them.
//
// **Degrades to the local path** — a single result partition (it arrived
// inline), a single-node cluster, an inline connector config rather than a
// saved connection, or no attached cluster: the whole result is fetched here and
// landed here, still as fetch-affinity, partition-only partitions.
//
// See docs/data/distributed-connector-ingest.md.
namespace zetta::connect {

// The `run.launch` kind this module registers. One registration, no bespoke
// fan-out path (docs/compute/coordinated-launch.md).
inline constexpr std::string_view kConnectorFetchLaunchKind = "connector_fetch";

// The collective tag prefix the participants agree over, suffixed with the
// launch id so concurrent ingests never share a round.
inline constexpr std::string_view kConnectorFetchCollectiveTag =
    "connector-fetch:";

// How long a participant waits for the rest of the cluster to report in before
// failing the ingest. Generous: it has to cover the slowest node's whole tile.
inline constexpr std::chrono::milliseconds kDefaultIngestCoordination{120000};

// What to ingest and where it lands.
struct DistributedIngestOptions {
    // The **saved connection name** (connection_store.hpp). Required for the
    // coordinated path — it is what lets every node present the credential
    // itself instead of the token riding the launch envelope. Empty means "run
    // the local path with whatever config the caller resolved".
    std::string connection;
    std::string query;
    std::string database = "datasets";
    std::string table_name;
    // **How many partitions the whole dataset gets — `nullopt` = auto** (#833).
    // The [`PartitionScheduler`](../data/partition_scheduler.hpp) decides it from
    // a target body size and a per-node floor, weighted by live telemetry; a
    // value here is a manual override, honoured exactly.
    std::optional<std::uint32_t> partition_count;
    std::size_t max_rows = 100000;       // cap pulled from the source, per node
    std::size_t replication_factor = 1;  // a result set is a cache: RF=1
    std::chrono::milliseconds coordination_timeout = kDefaultIngestCoordination;
    // The policy that decides the plan; null = `default_partition_scheduler()`.
    const data::PartitionScheduler* scheduler = nullptr;
};

// The outcome on the node that ran the ingest.
struct DistributedIngestResult {
    bool coordinated = false;  // peers co-fetched (false = the local path)
    std::vector<std::string> participants;
    std::size_t partitions_fetched = 0;  // source chunks THIS node pulled
    std::size_t rows = 0;                // rows landed CLUSTER-WIDE
    std::vector<std::string> columns;
    std::shared_ptr<data::Database> database;
    std::shared_ptr<data::Table> table;
    // The catalog entries this node wrote (a peer's are its own).
    std::vector<std::shared_ptr<data::DatasetPartition>> partitions;
    // The plan the scheduler emitted and the initiator SHIPPED to every
    // participant — the count, the size targets, the owners, and the rationale.
    // Returned so the decision is observable rather than a number nobody can
    // account for. On a peer this is the plan it received, byte for byte.
    data::PartitionPlan plan;
    std::string job;    // the ComputeJob id (system.jobs)
    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// Run the ingest from `node`: submit, co-launch, fetch this node's tile, land
// it, and agree with the rest of the roster. `config` is the resolved connector
// configuration (with the credential) this node uses for its own requests;
// `options.connection` names the saved connection the *peers* resolve for
// themselves. Non-throwing — every failure comes back in `error`, with any
// partitions this node wrote already discarded.
DistributedIngestResult run_distributed_connector_ingest(
    Node& node, const ConnectorConfig& config,
    const DistributedIngestOptions& options);

// The `run.launch` payload schema (draft-07) — the **code-owned source of
// truth**; `schemas/connector_fetch_launch.schema.json` is generated from it and
// drift-checked by tests/test_schemas.cpp. Validated before a runner reads a
// field, so a malformed or hostile envelope never reaches the fetch. Note what
// is *not* in it: no token, no account, no URL — only a saved-connection name.
const nlohmann::json& connector_fetch_launch_schema();

// Register the `connector_fetch` runner on `listener` (called by
// `Node::attach_cluster`, beside the churn one). The runner derives this node's
// rank from the roster, fetches only its tile, lands it, and joins the
// agreement — on the node-owning thread, like every other launch kind.
void install_connector_fetch_launch_runner(
    compute::CoordinatedLaunchListener& listener, Node& node);

}  // namespace zetta::connect
