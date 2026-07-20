#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/connect/connector.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/runtime.hpp"

// **The distributed connector fetch** — pull one query's result with the
// cluster's aggregate bandwidth instead of one node's
// ([#827](https://github.com/oramasearch/zetta/issues/827)).
//
// A warehouse hands a large result back as **N independently-fetchable result
// partitions** (Snowflake's SQL API v2 returns `resultSetMetaData.partitionInfo[]`
// beside the statement handle, each partition fetched with
// `GET /statements/{handle}?partition=i`). Reading them one after another on one
// node makes the ingest as slow as that node's single link to the source — the
// exact shape the repo's distributed-first rule forbids ("never add an algorithm
// that structurally can't distribute").
//
// The result set is therefore treated as a **work set to be tiled**:
//
//   1. **Submit once.** One node POSTs the statement and reads back the handle
//      plus the per-partition row counts (`StatementPlan`).
//   2. **Tile it deterministically.** `plan_partition_assignments` splits the
//      partition indices into disjoint per-node slices **balanced by row count**,
//      not by index — result partitions are uneven, so round-robin would leave
//      one node holding the tail. The plan is a pure function of (roster, row
//      counts), so every node computes the identical tiling from the same inputs
//      and no assignment has to travel.
//   3. **Fetch concurrently.** Each node pulls its own slice with several
//      requests in flight at once (`fetch_result_partitions`), through the
//      injected coroutine transport — `runtime::when_all` over a bounded batch, each
//      exchange under `runtime::timeout`. The bound matters: a TLS handshake
//      with full chain verification is not free, and an unbounded fan-out would
//      open a socket per partition.
//
// **Bounded, not unbounded.** The fan-out runs in waves of `max_in_flight`, so a
// thousand-partition result costs a fixed number of concurrent sockets.
//
// See docs/data/connector-query.md.
namespace zetta::connect {

// What a submitted statement hands back: the handle its partitions are fetched
// off, and the shape of the result set.
struct StatementPlan {
    std::string handle;
    // Row count per result partition, in partition order. Partition 0's rows
    // arrive inline with the submission; the rest are fetched by index.
    std::vector<std::uint64_t> partition_rows;
    std::vector<std::string> columns;
    std::vector<std::string> column_types;  // declared rowType.type, lowercased

    // The partitions that still have to be fetched (everything past 0).
    std::size_t fetchable_partitions() const {
        return partition_rows.size() > 1 ? partition_rows.size() - 1 : 0;
    }

    // Rows the SOURCE says the whole result set carries, across every partition
    // — known before a single one is pulled, which is what lets the partition
    // scheduler size the dataset up front rather than after the fact.
    std::uint64_t source_rows() const {
        std::uint64_t total = 0;
        for (const std::uint64_t rows : partition_rows) total += rows;
        return total;
    }
};

// One node's slice of the result set: a disjoint set of partition indices and
// the rows they are expected to carry.
struct PartitionAssignment {
    std::string node;
    std::vector<std::size_t> partitions;  // ascending
    std::uint64_t rows = 0;               // the slice's expected row count
};

// Tile `partition_rows`' indices across `nodes`, balanced by row count.
//
// **Deterministic**: a pure function of its inputs, so every node computes the
// same tiling and the assignment never has to be sent. **Disjoint and total**:
// every index in `[0, partition_rows.size())` appears in exactly one node's
// slice — no duplication, no loss. Partition 0 is included; a caller that
// already has it inline drops it from its own slice.
//
// The balance is longest-processing-time-first (largest partition to the
// currently-lightest node), which is the standard greedy makespan heuristic —
// the right one here because a node's fetch time tracks the rows it pulls, and
// partitions are uneven. Ties break on node order so the result stays stable.
//
// An empty `nodes` yields an empty plan; every node gets an entry even when its
// slice is empty, so a caller can report the full tiling.
//
// `node_weights` is the **share of the dataset each node is meant to end up
// with**, in `nodes` order — the [partition plan](../data/partition_scheduler.hpp)'s
// per-node primary count. It is what makes the fetch land in the target shape
// rather than in an evenly-tiled one that then has to be shuffled: a node that
// will own a fifth of the partitions pulls a fifth of the rows. Empty (the
// default) means equal shares, which is the previous behaviour exactly. A node
// weighted 0 is given nothing; if every weight is 0 the weights are ignored.
std::vector<PartitionAssignment> plan_partition_assignments(
    const std::vector<std::string>& nodes,
    const std::vector<std::uint64_t>& partition_rows,
    const std::vector<double>& node_weights = {});

// Tile the **fetchable** result partitions across `roster` — what the cross-node
// co-launch actually runs on (#833 section 7).
//
// The difference from `plan_partition_assignments` is partition **0**: it
// arrives *inline with the submission*, so the node that submitted the statement
// already holds it. Tiling it would mean either fetching those bytes a second
// time (they crossed the wire once already) or discarding them. So partition 0
// is excluded here and stays with the initiator, and `[1, N)` is split by the
// same row-balanced longest-processing-time-first rule over the whole roster.
//
// **`node_weights` shapes the fetch to the target layout** — the partition
// plan's per-node primary count, in roster order. A Snowflake result set is a
// *flexible* source: its chunks can go to whichever node should own those rows,
// so the fetch itself is laid out in the resting shape and there is **nothing to
// rebalance afterwards**. Empty weights mean equal shares (the pre-plan
// behaviour), which is what the local, single-node path uses.
//
// Deterministic given its inputs. It is no longer *derivable independently*,
// though: the weights come from a capacity-aware plan computed against live
// telemetry, which differs per node, so the coordinator computes the plan once
// and ships it in the launch payload and participants pass the received weights
// in here. The returned indices are in the ORIGINAL partition space (so `1`
// means result partition 1); `rows` counts only the partitions in the slice. A
// result with one partition (or none) yields empty slices for every node.
std::vector<PartitionAssignment> plan_fetch_assignments(
    const std::vector<std::string>& roster,
    const std::vector<std::uint64_t>& partition_rows,
    const std::vector<double>& node_weights = {});

// The outcome of submitting a statement: the plan, plus partition 0's rows
// (which the source returns inline with the submission).
struct StatementSubmission {
    StatementPlan plan;
    ConnectorRows inline_rows;
    std::string error;

    bool ok() const { return error.empty(); }
};

// POST `sql` to the source described by `config` and read back the plan +
// partition 0. Applies no read-only guard of its own — `run_connector_query` has
// already run it. Non-throwing; a failure comes back in `error`.
//
// **The 202 poll is a timer, not a sleep** (#832). The SQL API answers 202 while
// a statement is still executing, so the handle has to be re-read until it
// completes. The coroutine below suspends on `runtime::delay` between reads, so
// a waiting caller occupies a timer slot rather than a thread; the synchronous
// entry point is the one explicit `runtime::block_on` bridge for callers that
// are not themselves coroutines (the same shape `fetch_result_partitions` uses).
// Parameters are by value: they live in the coroutine frame.
runtime::task<StatementSubmission> submit_connector_statement_async(
    ConnectorConfig config, std::string sql, std::size_t max_rows,
    HttpTransport transport);

StatementSubmission submit_connector_statement(const ConnectorConfig& config,
                                               const std::string& sql,
                                               std::size_t max_rows,
                                               const HttpTransport& transport);

// Rows pulled from a set of result partitions.
struct PartitionFetchResult {
    std::vector<nlohmann::json> rows;
    std::size_t partitions_fetched = 0;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Fetch `partitions` of `plan` from `config`'s source **concurrently**, in waves
// of at most `max_in_flight`, stopping once `max_rows` rows are in hand. Rows
// come back typed by the plan's column types, in partition order within a wave.
//
// Requires the async transport (an empty one is a clean failure, not a crash):
// the transport's coroutine shape exists precisely so the requests overlap. Every
// exchange carries the request's own timeout, so one stuck partition cannot pin
// the wave.
PartitionFetchResult fetch_result_partitions(const ConnectorConfig& config,
                                             const StatementPlan& plan,
                                             const std::vector<std::size_t>& partitions,
                                             std::size_t max_rows,
                                             std::size_t max_in_flight,
                                             const AsyncHttpTransport& transport);

// The **degrade path**: walk `partitions` one after another over the synchronous
// transport. Correct, just not concurrent — used when no async client is
// installed (a unit test, an egress-free build) or when the caller injected its
// own transport, which must then be the only thing that speaks. Same result
// shape and same fail-fast-on-one-partition contract as the concurrent form.
PartitionFetchResult fetch_result_partitions_serially(
    const ConnectorConfig& config, const StatementPlan& plan,
    const std::vector<std::size_t>& partitions, std::size_t max_rows,
    const HttpTransport& transport);

// How many partition fetches one node keeps in flight by default. Small on
// purpose: each is a fresh TLS handshake plus a large body, so the win flattens
// quickly while the socket and memory cost keeps climbing.
inline constexpr std::size_t kDefaultPartitionsInFlight = 4;

}  // namespace zetta::connect
