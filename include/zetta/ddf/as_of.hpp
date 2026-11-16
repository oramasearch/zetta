// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/ddf/data_frame.hpp"

// The DDF window / as-of (point-in-time) operator — the first op over the DDF
// DataFrame substrate (ARC-12, #384; part of the native distributed accelerated-
// compute pass for UC-1 churn, #264). It is the **leakage-free featurization
// primitive**: for each entity, reduce the values of only the rows whose
// timestamp falls at or before an `as_of` cutoff, so a feature computed "as of
// t0" can never see an event that happened after t0.
//
// A churn feature like "number of payment failures before the decision date" or
// "total support severity in the trailing 90 days" is exactly an as-of /
// windowed group-by: group rows by entity, keep only rows inside the time window,
// reduce their values. Doing this with a hard time cutoff is what prevents the
// classic ML leak where a post-cutoff event sneaks into a feature and inflates
// the apparent signal.
//
// The DataFrame is **numeric** (zetta::compute::DataType — float / int / uint
// only), so this op operates over numeric columns: an integer **entity-key**
// column (the group key), a numeric **timestamp** column (e.g. epoch seconds, or
// any monotonic encoding of time), and a numeric **value** column to reduce. A
// featurize step upstream converts string ids / ISO-8601 timestamps into those
// numeric columns; this op stays purely numeric.
//
// Like the rest of the compute seam it is **non-throwing**: it returns a failing
// `zetta::compute::ComputeStatus` (never a throw across the zetta-core seam) on a
// missing / wrong-typed column or an empty time window. This slice is the host-
// side reference implementation (correctness over throughput, like CpuDevice is
// the reference for CudaDevice); pushing the group-by onto the device's
// sort_by_key + segmented-scan operators is a later optimization behind the same
// API. See docs/ddf/ddf-as-of.md.
namespace zetta::ddf {

// One as-of / windowed aggregation request over a (numeric) DataFrame.
//
// A row qualifies (contributes to its entity's aggregate) iff its timestamp is at
// or before `as_of` AND — when `window_start` is set — at or after `window_start`.
// In interval terms: `time <= as_of` (and `time >= window_start`), so the window
// is the closed interval [window_start, as_of], or the unbounded-below
// (-inf, as_of] when `window_start` is absent (the pure point-in-time case).
struct AsOfAggregate {
    std::string entity_column;  // integer-dtype column whose value groups the rows
    std::string time_column;    // numeric column carrying each row's timestamp
    std::string value_column;   // numeric column reduced per entity over the window
    double as_of = 0.0;         // upper cutoff (inclusive): a row qualifies iff time <= as_of
    std::optional<double> window_start;  // optional lower cutoff (inclusive): time >= window_start
    compute::ReduceOp op = compute::ReduceOp::Sum;  // how qualifying values combine

    // The JSON value shape:
    //   { "entity_column", "time_column", "value_column", "as_of",
    //     "window_start"?, "op" }
    // where `op` is one of "sum" / "product" / "minimum" / "maximum", and
    // `window_start` is omitted when unset. Mirrors
    // schemas/as_of_aggregate.schema.json — keep them in sync.
    nlohmann::json to_json() const;

    // Parse + validate at the boundary; the non-throwing inverse of to_json
    // (std::optional, like ScalingPolicy::from_json / DataFrameSchema::from_json).
    // Returns std::nullopt unless `value` is an object with non-empty string
    // entity_column / time_column / value_column, a numeric `as_of`, a known `op`
    // name, an optional numeric `window_start` that is <= `as_of`, and no unknown
    // keys.
    static std::optional<AsOfAggregate> from_json(const nlohmann::json& value);

    bool operator==(const AsOfAggregate&) const = default;
};

// The AsOfAggregate JSON Schema (draft-07) — the code-owned source of truth;
// schemas/as_of_aggregate.schema.json is generated from it and drift-checked
// by tests/test_schemas.cpp (#646).
const nlohmann::json& as_of_aggregate_schema();

// The result of an as-of aggregation: one entry per distinct entity present in the
// frame, in ascending entity-key order (deterministic). For entity `entities[i]`:
//   - `values[i]`  = the reduction (op) over that entity's qualifying rows, or the
//                    reduction identity (Sum -> 0, Product -> 1, Minimum -> +inf,
//                    Maximum -> -inf) when no row qualifies;
//   - `counts[i]`  = the number of that entity's rows inside the window.
// The output is **dense**: an entity whose every row is in the future still appears
// (with count 0 and the identity value), so callers get a feature row per entity.
struct AsOfColumn {
    std::vector<std::int64_t> entities;
    std::vector<double> values;
    std::vector<std::int64_t> counts;

    std::size_t size() const noexcept { return entities.size(); }
    bool empty() const noexcept { return entities.empty(); }
};

// The outcome of `as_of_aggregate`: the per-entity column (populated only when
// `status.ok()`) plus the status that explains any failure.
struct AsOfResult {
    AsOfColumn column;
    compute::ComputeStatus status;

    bool ok() const noexcept { return status.ok(); }
};

// Compute an as-of / windowed aggregation over `frame` per `spec`.
//
// Non-throwing. Fails with InvalidArgument when: a named column is absent; the
// entity column is not an integer dtype (the group key must be exact); the time or
// value column is non-numeric (every DataFrame column is numeric, so this only
// guards a future widening); or `window_start` is set above `as_of` (an empty
// window). A device read-back error is surfaced as that device's status. On
// success every distinct entity in the frame appears in the result, in ascending
// key order, with future / out-of-window rows excluded.
AsOfResult as_of_aggregate(const DataFrame& frame, const AsOfAggregate& spec);

}  // namespace zetta::ddf
