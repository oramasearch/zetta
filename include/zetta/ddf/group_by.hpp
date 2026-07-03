#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_status.hpp"
#include "zetta/ddf/data_frame.hpp"

// DDF distributed groupby-aggregate (ARC-12, #383) — the first DDF *operator* over
// the [DataFrame core](data_frame.hpp). It groups a frame's rows by a numeric key
// column and computes aggregates (count / sum / min / max / mean, an extensible
// set) over each group, **distributed**: every node computes a partial aggregate
// over its own shard, then the partials are combined across the cluster so every
// node ends with identical results.
//
// The combine rides the [collectives](../collectives.md) (#365): a node's local
// partial is serialized to a flat vector<double>, **gathered** to the deterministic
// root, merged there (count/sum/min/max all combine associatively; mean carries a
// running sum + count and is divided only at finalize), then the merged partial is
// **broadcast** back — so the round ends with bit-identical bytes on every node
// (gather + broadcast is the natural fit for per-node group sets that differ).
//
// Numeric only, like the DataFrame: keys and aggregated values are read back
// through the ComputeDevice download seam as doubles, so the op is **backend-
// agnostic** (it runs the same over a CPU frame or a CUDA frame). Group keys must
// be finite numbers (a NaN key is rejected at the boundary).
//
// Non-throwing throughout (zetta-core's no-exceptions convention): every entry
// point returns a ComputeStatus alongside its result. See docs/ddf/ddf-groupby.md.
namespace zetta::ddf {

// --- Aggregate operators -----------------------------------------------------

// The aggregate computed over each group. An extensible set: a new op is added
// here and handled in the accumulate / merge / finalize switches in group_by.cpp.
enum class AggregateOp { Count, Sum, Min, Max, Mean };

// The lowercase canonical name of an op ("count"/"sum"/"min"/"max"/"mean").
std::string_view to_string(AggregateOp op);

// Parse a name back to an AggregateOp (nullopt on anything unknown).
std::optional<AggregateOp> aggregate_op_from_string(std::string_view name);

// Whether `op` aggregates a column's values (everything but Count, which just
// counts the group's rows and needs no column).
bool aggregate_needs_column(AggregateOp op);

// One aggregate to compute over a grouped frame.
struct Aggregate {
    AggregateOp op = AggregateOp::Count;
    std::string column;   // the frame column aggregated; ignored/empty for Count
    std::string output;   // result column name; defaulted when empty

    // The effective output column name: `output` when set, else "count" for Count
    // or "<op>_<column>" otherwise (e.g. "sum_amount", "mean_score").
    std::string output_name() const;
};

// The specification of a groupby-aggregate, with the JSON boundary it crosses.
// Mirrors schemas/group_by_spec.schema.json.
struct GroupBySpec {
    std::string key;                     // the numeric column whose values group rows
    std::vector<Aggregate> aggregates;   // the aggregates to compute per group

    // The JSON form: { "key", "aggregates":[ { "op", "column"?, "output"? }, ... ] }.
    nlohmann::json to_json() const;

    // Parse + validate a spec at the boundary; the non-throwing inverse of to_json
    // (std::optional, like DataFrameSchema::from_json). std::nullopt unless `value`
    // is an object with a non-empty string `key` and a non-empty `aggregates` array
    // of objects, each having a known `op`, an optional non-empty string `output`,
    // and a non-empty string `column` whenever the op aggregates a column
    // (sum/min/max/mean).
    static std::optional<GroupBySpec> from_json(const nlohmann::json& value);
};

// --- The result --------------------------------------------------------------

// The result of a groupby-aggregate: one row per distinct group key, the keys in
// ascending order (so the result is deterministic regardless of row or node order).
// Columnar: `keys[g]` is group g's key, and `values[a][g]` is aggregate a's value
// for group g.
struct GroupByResult {
    std::string key_name;                       // the grouping column's name
    std::vector<std::string> aggregate_names;   // one per aggregate, in spec order
    std::vector<double> keys;                   // distinct group keys, ascending
    std::vector<std::vector<double>> values;    // values[aggregate][group]

    std::size_t group_count() const noexcept { return keys.size(); }
    std::size_t aggregate_count() const noexcept { return aggregate_names.size(); }

    // The value column for an aggregate by its output name, or nullptr if absent.
    const std::vector<double>* value(std::string_view aggregate_name) const;
};

// --- The mergeable partial (the per-node "partial aggregate") ----------------

// A node's partial groupby state: per group, the row count plus one running
// accumulator per aggregate (sum, min, max, or running sum for mean). Mergeable
// across nodes — this is the partial the distributed algorithm combines. Carries
// the aggregate `ops` so a merge is op-aware. A plain struct (a DTO), like
// CollectiveMessage / ColumnSchema.
struct GroupByPartial {
    // The aggregate ops, in order — the i-th op governs accumulator slot i.
    std::vector<AggregateOp> ops;
    // key -> accumulators, laid out [count, acc_0, ..., acc_{ops.size()-1}].
    std::map<double, std::vector<double>> groups;

    std::size_t group_count() const noexcept { return groups.size(); }
    bool empty() const noexcept { return groups.empty(); }

    // Combine `other` into this partial (ops must match). count adds; per aggregate
    // sum/mean add the running sum, min/max combine. Returns false (a no-op) on an
    // ops mismatch, true otherwise.
    bool merge(const GroupByPartial& other);

    // The flat wire form a collective carries:
    //   [ aggregate_count, group_count, (key, count, acc_0..acc_{k-1}) * groups ]
    // groups in ascending key order, so the bytes are deterministic.
    std::vector<double> to_wire() const;

    // Parse a wire form (produced by to_wire) with the expected `ops`. The boundary
    // for a collective-carried partial: nullopt on any shape/count mismatch, a
    // duplicate key, or a NaN key.
    static std::optional<GroupByPartial> from_wire(std::vector<AggregateOp> ops,
                                                   const std::vector<double>& wire);

    // Finalize into a result (mean = running sum / count), groups ascending by key.
    GroupByResult finalize(const GroupBySpec& spec) const;
};

// --- Entry points ------------------------------------------------------------

// The outcome of a groupby-aggregate: the result (populated only when status.ok())
// plus the status that explains any failure. Mirrors DataFrameResult.
struct GroupByOutcome {
    GroupByResult result;
    compute::ComputeStatus status;

    bool ok() const noexcept { return status.ok(); }
};

// The outcome of computing a node's local partial.
struct GroupByPartialOutcome {
    GroupByPartial partial;
    compute::ComputeStatus status;

    bool ok() const noexcept { return status.ok(); }
};

// Compute a node's local partial over `frame`'s columns — the per-shard partial of
// the distributed path (and the heart of the single-node group_by). Non-throwing:
// a failing ComputeStatus (and an empty partial) if the key/aggregate columns are
// absent, an aggregate that needs a column has none, or a key is NaN.
GroupByPartialOutcome local_partial(const DataFrame& frame, const GroupBySpec& spec);

// Single-node groupby-aggregate over `frame`: local_partial then finalize.
GroupByOutcome group_by(const DataFrame& frame, const GroupBySpec& spec);

// The default per-collective timeout for a distributed round (a generous 10s, like
// Collectives::kDefaultTimeout — kept local so this header need not pull in the
// collectives transport).
inline constexpr std::chrono::milliseconds kDefaultGroupByTimeout{10000};

// Distributed groupby-aggregate: combine every participant's local partial into a
// single result identical on every node. `Collective` is any type exposing the
// signal-plane collectives gather + broadcast (zetta::Collectives and
// zetta::compute::JobRound both do), so a job body can call this directly with its
// JobRound. SPMD: every node calls it with the same `spec` and `tag` over its own
// shard.
//
// The round: serialize the local partial -> gather to the root -> the root merges
// every node's partial -> broadcast the merged partial -> every node finalizes it.
// Non-throwing: a failing ComputeStatus on a gather/broadcast failure (unreachable
// peer or timeout), a malformed gathered/broadcast partial, or a local error.
template <class Collective>
GroupByOutcome distributed_group_by(
    const DataFrame& frame, const GroupBySpec& spec, Collective& collective,
    const std::string& tag,
    std::chrono::milliseconds timeout = kDefaultGroupByTimeout) {
    const GroupByPartialOutcome local = local_partial(frame, spec);
    if (!local.ok()) return {GroupByResult{}, local.status};

    // Gather every node's partial to the deterministic root (lowest node id). The
    // root receives the ordered list; non-root callers receive an empty list.
    const std::optional<std::vector<std::vector<double>>> gathered =
        collective.gather(tag + ".gather", local.partial.to_wire(), timeout);
    if (!gathered) {
        return {GroupByResult{},
                compute::ComputeStatus::backend_error(
                    "distributed_group_by: gather failed (peer unreachable or "
                    "timeout)")};
    }

    // The root merges the partials; everyone else broadcasts an empty placeholder
    // (broadcast distributes only the root's vector).
    std::vector<double> merged_wire;
    if (!gathered->empty()) {
        GroupByPartial merged;
        merged.ops = local.partial.ops;
        for (const std::vector<double>& node_wire : *gathered) {
            const std::optional<GroupByPartial> piece =
                GroupByPartial::from_wire(merged.ops, node_wire);
            if (!piece) {
                return {GroupByResult{},
                        compute::ComputeStatus::invalid_argument(
                            "distributed_group_by: a gathered partial was "
                            "malformed")};
            }
            merged.merge(*piece);
        }
        merged_wire = merged.to_wire();
    }

    const std::optional<std::vector<double>> result_wire =
        collective.broadcast(tag + ".result", merged_wire, timeout);
    if (!result_wire) {
        return {GroupByResult{},
                compute::ComputeStatus::backend_error(
                    "distributed_group_by: broadcast failed (peer unreachable or "
                    "timeout)")};
    }

    const std::optional<GroupByPartial> merged =
        GroupByPartial::from_wire(local.partial.ops, *result_wire);
    if (!merged) {
        return {GroupByResult{},
                compute::ComputeStatus::invalid_argument(
                    "distributed_group_by: the broadcast result was malformed")};
    }
    return {merged->finalize(spec), compute::ComputeStatus::success()};
}

}  // namespace zetta::ddf
