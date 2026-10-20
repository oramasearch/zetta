#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/signal.hpp"
#include "zetta/query/ast.hpp"

// **Scatter-gather SELECT over a partition-only table** (#833 section 5).
//
// SQL is node-local: the executor reads `table->store()`. That is only *complete*
// because a partitioned table today also replicates every row through the CRDT.
// A [partition-only table](../../../docs/data/partition-only-tables.md) does not —
// its rows live in the partition bodies their owners hold — so a node-local scan
// would answer from whatever slice happened to be here. `SELECT count(*)` would
// come back a fraction of the truth and look perfectly healthy.
//
// So a SELECT over such a table fans out to the partition **owners** and merges,
// following the `fulltext.query` precedent (`zetta::index::DistributedFulltextIndex`):
// one RPC verb on the signal plane, a handler chained over the cluster's existing
// one, schema-validated payloads, and a fail-fast gather.
//
// Two things it does differently from that precedent, both forced by SQL being
// exact where ranking is not:
//
//   * **Each partition is scanned exactly once.** Fulltext scatters to every peer
//     and lets each filter to what it owns, which is safe only while every
//     partition has a single owner. With replicas, that would count a partition
//     once per owner. Here the coordinator assigns every partition to ONE
//     reachable owner and sends each responder its list.
//   * **An unreachable owner fails the query.** With RF=1 on connector data there
//     is no replica to fall back on, so answering from four shards out of five is
//     the one outcome that must not ship. The error names the partition and its
//     owners; the remedy is re-running the ingest (or promoting the dataset to a
//     higher replication factor first).
//   * **An incomplete catalog fails the query too** (#837). A partition whose
//     `part:` entry has not gossiped here yet is not unreachable — it is
//     *invisible*, so the guard above never fires and the plan is silently short.
//     Before scattering, the coordinator counts the entries it can see against the
//     dataset's [partition manifest](../data/dataset_manifest.hpp) and fails when
//     it falls short, with the same "the dataset is degraded" shape.
//
// **Aggregates merge, they do not concatenate.** A responder returns a per-group
// PARTIAL — `{value, count}` per aggregate — not a finished answer, because a
// finished `avg` cannot be merged: averaging the averages weights a 1-row shard
// like a 10 000-row one. `count`/`sum` add, `min`/`max` reduce, and `avg` divides
// the summed value by the summed count at the end.
//
// **Threading.** `on_request` runs on the cluster's network thread, where the Node
// tree is off limits. The scan therefore reads a snapshot of the owned partitions
// (`refresh()`, called from `Node::tick()` and before every scatter) plus the body
// files those entries name — bodies are content-addressed and written once, so
// reading one by path off-thread is safe. Nothing here throws.
//
// See docs/data/scatter-gather-select.md.
namespace zetta { class Node; }
namespace zetta::cluster { class Cluster; }

namespace zetta::query {

// One aggregate a scan must compute, in the select list's order.
struct ScanAggregate {
    AggregateFunc func = AggregateFunc::Count;
    std::string column;  // empty for COUNT(*)
};

// What a responder is asked to do: scan these partitions of this table, filter
// them, and return either the surviving rows or per-group aggregate partials.
struct PartitionScanRequest {
    std::string table;                        // the `tbl:` id the catalog records
    std::vector<std::string> partitions;      // catalog entry ids (`part:` uuids)
    std::optional<WhereClause> where;         // applied at the owner (near the data)
    std::vector<std::string> columns;         // projection; empty = the whole row
    std::string group_by;                     // the group key; empty = one group
    std::vector<ScanAggregate> aggregates;    // the aggregates, in select order

    // **LIMIT pushdown** (#1021), non-aggregating scans only. `keep` is the most
    // rows any one partition may return: the statement's `offset + limit`, since
    // the global answer can need that many from a single partition and never
    // more. With `order_by`, a partition sorts its surviving rows by those keys
    // (the same stable order the coordinator uses) and keeps the first `keep`;
    // without it, the first `keep` in row order. The coordinator still applies
    // ORDER BY / OFFSET / LIMIT to the merged rows, which gives exactly the
    // answer a full scan would: the global top `keep` of a stable sort is, per
    // partition, a prefix of that partition's own stable sort.
    std::vector<OrderKey> order_by;
    std::optional<std::size_t> keep;

    // An aggregating scan returns per-group partials instead of rows: it has a
    // group key, an aggregate, or both. With aggregates and no key, the whole
    // scan is one group (`SELECT count(*) FROM t`, #1218).
    bool aggregating() const { return !group_by.empty() || !aggregates.empty(); }
};

// One partition's surviving rows, kept separate so the coordinator can emit rows
// in the dataset's row order regardless of which node answered.
struct ScanBlock {
    std::string partition;              // the catalog entry id
    std::vector<nlohmann::json> rows;
};

// One aggregate's partial for one group. `count` is how many rows contributed —
// the weight `avg` needs and the emptiness test `min`/`max` need. `integral`
// records that every contributing value was a JSON integer, so a merged `sum`
// of integers still reads back as an integer.
struct AggregatePartial {
    double value = 0.0;
    std::uint64_t count = 0;
    bool integral = true;
};

// One group's partials, in the request's aggregate order.
struct GroupPartial {
    nlohmann::json key;
    std::vector<AggregatePartial> aggregates;
};

// What a responder sends back.
struct PartitionScanResponse {
    std::string from;                    // the responding node id
    std::uint64_t partitions_read = 0;
    std::uint64_t rows_scanned = 0;      // rows read before the WHERE
    std::uint64_t bytes_read = 0;        // partition-body bytes read from local disk
    std::vector<ScanBlock> blocks;       // non-aggregating scans
    std::vector<GroupPartial> groups;    // aggregating scans
    // Every column any scanned row carried (before the WHERE), in first-seen
    // order — what the coordinator checks a named column against, since a
    // schemaless table has no declared columns (#1218). Unset when the responder
    // predates it, and then the coordinator does not check.
    std::optional<std::vector<std::string>> columns_seen;
    std::string error;                   // empty on success

    bool ok() const { return error.empty(); }
};

std::string encode_partition_scan_request(const PartitionScanRequest& request);
std::optional<PartitionScanRequest> decode_partition_scan_request(
    std::string_view payload, std::string* error = nullptr);
const nlohmann::json& partition_scan_request_schema();

std::string encode_partition_scan_response(const PartitionScanResponse& response);
std::optional<PartitionScanResponse> decode_partition_scan_response(
    std::string_view payload, std::string* error = nullptr);
const nlohmann::json& partition_scan_response_schema();

// One node's part in a scattered SELECT — the per-node provenance the
// observability rule wants visible.
struct SelectContribution {
    std::string node_id;
    bool local = false;
    std::uint64_t partitions = 0;
    std::uint64_t rows_scanned = 0;
    double latency_ms = 0.0;
    std::uint64_t wire_bytes = 0;
};

// The merged answer. `rows` are the result rows (already merged, and for an
// aggregating query already divided/reduced); `columns` names them when the
// query projected. On failure `error` says why and `rows` is empty — there is no
// partial answer.
struct SelectOutcome {
    std::vector<std::string> columns;
    std::vector<nlohmann::json> rows;
    // The columns the scanned partitions hold (the union of every responder's
    // `columns_seen`), and whether every responder reported them — only then is
    // a column missing from the list known not to exist.
    std::vector<std::string> table_columns;
    bool table_columns_complete = false;
    std::uint64_t partitions_read = 0;
    std::uint64_t rows_scanned = 0;
    // Rows the partitions sent back (non-aggregating scans): with a pushed-down
    // LIMIT (#1021) at most `offset + limit` per partition, however many rows
    // they scanned.
    std::uint64_t rows_received = 0;
    std::vector<SelectContribution> contributions;
    std::string error;

    bool ok() const { return error.empty(); }
};

// The partitions of one table this node can read, with the on-disk path of each
// body — the network-thread-readable projection of the catalog.
struct OwnedPartition {
    std::string id;                  // catalog entry id
    std::uint64_t row_start = 0;     // orders the merged rows
    std::filesystem::path body;      // the large object's body file
};

// Folds rows, one at a time, into per-group aggregate partials. The one place
// SQL aggregation semantics live: the partition owner's scan, the executor's
// replicated-table path (text keys, whole-table aggregates) and the system and
// virtual tables all aggregate through it, so the same query answers the same
// way whichever path serves it (#1218). Group keys are JSON values — numbers,
// strings, booleans or null (a row without the key column) — and a row adds
// to every aggregate of its group. With no group key every row is one group.
class GroupAccumulator {
public:
    GroupAccumulator(std::string group_by, std::vector<ScanAggregate> aggregates);
    // The accumulator for a SELECT's aggregating select list.
    explicit GroupAccumulator(const SelectStmt& statement);

    void add(const nlohmann::json& row);

    // The partials, in first-seen group order; leaves the accumulator empty.
    std::vector<GroupPartial> take();

private:
    std::string group_by_;
    std::vector<ScanAggregate> aggregates_;
    std::unordered_map<std::string, std::size_t> index_;
    std::vector<GroupPartial> groups_;
};

// ORDER BY's comparison of two cells (-1, 0, 1): numbers before booleans before
// strings before anything else, and NULL (or a missing cell) last, as Postgres
// sorts ascending. One definition for the coordinator and every partition owner,
// so a pushed-down top-N keeps exactly the rows the merged sort would.
int compare_for_order(const nlohmann::json& left, const nlohmann::json& right);

// Stable-sort `rows` by `keys` (each ascending unless `descending`).
void sort_rows_for_order(std::vector<nlohmann::json>& rows,
                         const std::vector<OrderKey>& keys);

// The aggregates an aggregating SELECT computes, in select order.
std::vector<ScanAggregate> scan_aggregates_of(const SelectStmt& statement);

// The result column names of an aggregating SELECT, in select order: a key's
// own name (or its alias), and an aggregate's alias or default name (`count`,
// `sum_x`, `min_x`, `max_x`, `mean_x`).
std::vector<std::string> aggregate_result_columns(const SelectStmt& statement);

// Scan `request` against `owned` (the responder's own partitions) and produce the
// partial. Pure apart from reading the body files; used by the RPC handler, by a
// coordinator for its own share, and by a node with no cluster attached.
PartitionScanResponse scan_owned_partitions(
    const std::vector<OwnedPartition>& owned, const PartitionScanRequest& request);

// Merge responders' answers into the finished result. `partition_order` is the
// table's partition ids in the dataset's row order, so rows come back in that
// order no matter which node answered; `statement` supplies the select list.
//
// Exposed (rather than left inside the coordinator) because this is where the
// design's aggregate rules live and they are worth asserting directly: `count`
// and `sum` add, `min`/`max` reduce, and `avg` divides the summed value by the
// summed row count — never the mean of the shards' means, which would weight a
// one-row shard like a ten-thousand-row one.
SelectOutcome merge_scan_responses(const SelectStmt& statement,
                                   const std::vector<std::string>& partition_order,
                                   const std::vector<PartitionScanResponse>& answers);

// The scatter-gather coordinator + the `sql.scan` responder. One per clustered
// node, owned by the Node and chained over the cluster's existing signal handler
// (`Node::attach_cluster`), exactly like DistributedFulltextIndex.
class DistributedSelect : public zetta::cluster::SignalHandler {
public:
    static constexpr std::string_view kScanMethod = "sql.scan";
    static constexpr std::chrono::milliseconds kDefaultTimeout{15000};

    DistributedSelect(Node& node, zetta::cluster::Cluster& cluster,
                      zetta::cluster::SignalHandler* delegate = nullptr);
    ~DistributedSelect() override;

    DistributedSelect(const DistributedSelect&) = delete;
    DistributedSelect& operator=(const DistributedSelect&) = delete;

    // Re-read the catalog into the snapshot the network thread serves from.
    // **Owner thread only** — called from Node::tick() and before every scatter.
    // Cheap when nothing changed (a fingerprint guards the rebuild).
    void refresh();

    // Answer `statement` over the partition-only table `table_id`. Owner thread.
    SelectOutcome execute(const SelectStmt& statement, const std::string& table_id,
                          std::chrono::milliseconds timeout = kDefaultTimeout);

    // How many scans this node has served for a peer — makes "the work ran where
    // the data is" assertable rather than assumed.
    std::uint64_t scans_served() const;

    zetta::cluster::SignalResponse on_request(
        const zetta::cluster::Signal& request) override;
    void on_event(const zetta::cluster::Signal& event) override;
    void on_task(const zetta::cluster::Signal& task) override;

private:
    Node& node_;
    zetta::cluster::Cluster& cluster_;
    zetta::cluster::SignalHandler* delegate_;

    // The snapshot + its fingerprint, read by the network thread. Held only for
    // the cheap in-memory copy, never across an RPC or a body read.
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<OwnedPartition>> owned_;  // table id -> owned
    std::string fingerprint_;
    std::uint64_t scans_served_ = 0;
};

// Answer a SELECT over a partition-only table, with or without a cluster. With
// `plane` it scatters; without one (a standalone node) every partition must be
// owned here, and a partition owned elsewhere fails the query with the same
// unreachable-owner reason a dead peer would produce. Owner thread.
SelectOutcome select_over_partitions(Node& node, DistributedSelect* plane,
                                     const SelectStmt& statement,
                                     const std::string& table_id);

}  // namespace zetta::query
