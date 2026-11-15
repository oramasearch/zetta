// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/group_by.hpp"

// Point-in-time (leakage-free) **featurization** over the DDF DataFrame substrate
// (ARC-12 / FEAT-6, #388; part of the native distributed accelerated-compute pass
// for UC-1 churn, #264). This is the consumer the leaf DDF ops were built toward:
// it expresses the shape of the churn `churn_features.sql` as a DDF graph composed
// from the ops that landed before it —
//
//   * the [as-of / window op](as_of.hpp) (#384) — the leakage-free windowed
//     reduction (`event_ts <= as_of`, optional trailing window),
//   * the [groupby-aggregate op](group_by.hpp) (#383) — per-entity count / mean,
//   * the [filter op](ops.hpp) (#381) — restrict events to one type before a
//     windowed aggregate,
//   * the [equi-join op](join.hpp) (#382) — attach the static account dimension.
//
// **The non-negotiable property is no leakage.** Every feature for an entity at
// decision time `t0 = as_of` is computed from only the events with `time <= as_of`;
// the realized churn label and any post-t0 signal live only in the training target,
// never in a feature. A churn feature like "failed payments in the trailing 90 days"
// or "total support severity before t0" is exactly a windowed group-by: group rows
// by entity, keep only the rows inside the time window, reduce their values. Doing
// it with a hard `time <= as_of` cutoff is what stops the classic leak where a
// post-cutoff event sneaks into a feature and inflates the apparent signal.
//
// **Numeric, like the rest of DDF.** Both the events frame and the accounts
// (dimension) frame are numeric DataFrames: an integer entity-key column (present in
// both — the join key), a numeric timestamp column in the events frame, the numeric
// value / type-code columns the features reduce, and the numeric static columns +
// label in the accounts frame. A featurize step upstream encodes string ids /
// ISO-8601 timestamps / categorical event types into those numeric columns; this op
// stays purely numeric.
//
// **Left-join semantics, dense output.** The feature table has one row per entity in
// the *accounts* frame (the entity universe to score), in ascending key order. An
// account with no qualifying events still appears, with its event features filled to
// 0 (the SQL `COALESCE(agg, 0)`) — so callers always get a feature row per account.
//
// Non-throwing throughout (zetta-core's no-exceptions convention): every entry point
// returns a `compute::ComputeStatus` alongside its result. The distributed variant —
// the same featurization driven across the cluster through the
// [job coordinator](../compute/job_coordinator.hpp) as a recorded `featurize`
// `ComputeJob` — is `run_featurize_job` in featurize_job.hpp. See docs/ddf/ddf-featurize.md.
namespace zetta::ddf {

// --- The spec ----------------------------------------------------------------

// One point-in-time event feature: an aggregate over the rows of an entity whose
// timestamp falls inside the window [window_start, as_of] (so always `time <= as_of`,
// leakage-free), optionally restricted to the rows of a single event type. The `agg`
// vocabulary is the groupby-aggregate set (count / sum / min / max / mean); sum / min
// / max run through the as-of op, count / mean through a windowed group-by.
struct WindowedFeature {
    std::string name;                       // output feature column name (unique)
    AggregateOp agg = AggregateOp::Count;   // how qualifying rows reduce
    std::string value_column;               // events column reduced; empty for Count
    std::optional<double> window_start;     // trailing-window lower bound (inclusive)
    std::string type_column;                // optional numeric event-type column
    std::optional<double> type_value;       // the code rows must equal (with type_column)

    // Whether a type filter applies (type_column set and type_value present).
    bool has_type_filter() const noexcept {
        return !type_column.empty() && type_value.has_value();
    }
};

// A point-in-time featurization spec: per-entity windowed event features joined onto
// a static account dimension, all windowed against a single as-of cutoff. Mirrors
// schemas/featurize_spec.schema.json.
//
// **Static (carry-through) mode.** `event_features` may be empty — a featurization of
// already-engineered numeric columns (e.g. the saas-sales-conversations engineered
// columns: customer_engagement / sales_effectiveness / …, embeddings dropped) where
// each row is one entity. The `account_features` are carried through unchanged and
// `label_column` is the target; there is no windowed aggregation. The point-in-time
// guarantee still holds "where applicable": when a `time_column` is set, the row
// universe is filtered to `time <= as_of` (a row after the cutoff is excluded — no
// leakage); with no `time_column` there is no time dimension to filter. In this mode
// `time_column` is optional and `event_features` empty; with windowed event features
// (the churn shape) a `time_column` is required.
struct FeaturizeSpec {
    std::string entity_column;   // integer entity key in BOTH frames (the join key)
    std::string time_column;     // numeric timestamp column; optional in static mode
    double as_of = 0.0;          // t0: only rows/events with time <= as_of contribute

    std::vector<WindowedFeature> event_features;   // windowed per-entity event features
                                                   // (empty => static carry-through)

    // Static columns carried through unchanged from the accounts frame (mrr, seats,
    // tenure_days, …), joined onto the event features by entity key.
    std::vector<std::string> account_features;

    // Optional training-target column from the accounts frame (e.g. churned). Carried
    // into the output as the label y, never a feature. Empty => no label.
    std::string label_column;

    // The JSON form: { "entity_column", "time_column", "as_of",
    //   "event_features":[ { "name", "agg", "value_column"?, "window_start"?,
    //   "type_column"?, "type_value"? }, … ], "account_features"?:[…], "label_column"? }.
    nlohmann::json to_json() const;

    // Parse + validate a spec at the boundary; the non-throwing inverse of to_json
    // (std::optional, like AsOfAggregate::from_json / GroupBySpec::from_json).
    // std::nullopt unless `value` is an object with a non-empty string entity_column;
    // an optional non-empty time_column (required iff event_features are present)
    // carrying a numeric as_of when set; an optional event_features array of
    // well-formed features (non-empty name, known agg, a value_column whenever the agg
    // needs one, an optional window_start <= as_of, type_column and type_value
    // together or not at all); an optional account_features array of non-empty
    // strings; an optional non-empty label_column; at least one feature column (an
    // event feature or an account feature — a label alone is not a feature table); all
    // output column names (features + account columns) unique; and no unknown keys.
    static std::optional<FeaturizeSpec> from_json(const nlohmann::json& value);
};

// The FeaturizeSpec JSON Schema (draft-07) — the code-owned source of truth;
// schemas/featurize_spec.schema.json is generated from it and drift-checked
// by tests/test_schemas.cpp (#646).
const nlohmann::json& featurize_spec_schema();

// --- The result --------------------------------------------------------------

// A dense, host-side feature table: one row per entity (the account universe), in
// ascending key order. `features[c][r]` is feature column c's value for row r (the
// design matrix X); when a label is present, `labels[r]` is its target y. Plain data.
struct FeatureTable {
    std::vector<std::int64_t> entities;          // entity key per row, ascending
    std::vector<std::string> feature_names;      // feature columns (X), in spec order
    std::vector<std::vector<double>> features;   // features[column][row]
    std::string label_name;                      // empty when the spec carries no label
    std::vector<double> labels;                  // target y per row (empty when no label)

    std::size_t row_count() const noexcept { return entities.size(); }
    std::size_t feature_count() const noexcept { return feature_names.size(); }
    bool has_label() const noexcept { return !label_name.empty(); }

    // The value column for a feature by name, or nullptr if absent.
    const std::vector<double>* feature(std::string_view name) const;

    // Materialize a device-backed feature DataFrame for downstream training: the
    // entity-key column (Int64), each feature column (Float64), and the label column
    // (Float64) when present. Non-throwing — returns a failing status on a device
    // error. The columns round-trip through the storage loader (load_data_frame), so
    // no new buffer-construction path is introduced.
    DataFrameResult to_data_frame(compute::ComputeDevice& device,
                                  std::size_t chunk_rows = kDefaultChunkRows) const;
};

// The outcome of featurization: the table (populated only when status.ok()) plus the
// status that explains any failure. Mirrors DataFrameResult / GroupByOutcome.
struct FeaturizeResult {
    FeatureTable table;
    compute::ComputeStatus status;

    bool ok() const noexcept { return status.ok(); }
};

// --- The op ------------------------------------------------------------------

// Featurize `events` against the `accounts` dimension per `spec`, single-node.
//
// Composes the leaf DDF ops: each windowed event feature runs through the as-of op
// (sum/min/max) or a windowed group-by (count/mean) — optionally pre-filtered to one
// event type — and the per-entity results are attached to the static account
// dimension with the equi-join op. Output is dense over the accounts' entity universe
// (one row per account, missing event features filled to 0).
//
// **Static mode** (`spec.event_features` empty): no events are read — the engineered
// `account_features` (+ optional label) of the `accounts` frame are carried through,
// one row per entity. When `spec.time_column` is set the `accounts` rows are first
// point-in-time filtered to `time <= as_of` (a post-cutoff row is excluded — no
// leakage); with no `time_column` there is no time filter. (The `events` frame is
// ignored; a caller may pass the same frame for both.)
//
// Non-throwing. Fails with InvalidArgument when: a named column is absent; the entity
// column is not an integer dtype in either frame; the spec has no feature column; or a
// feature's window/value is malformed. A device read-back error is surfaced as that
// device's status. The distributed equivalent is run_featurize_job (featurize_job.hpp);
// the compute-near-data variant over a partitioned table is run_near_data_featurize_job
// (local_featurize_job.hpp).
FeaturizeResult point_in_time_featurize(const DataFrame& events,
                                        const DataFrame& accounts,
                                        const FeaturizeSpec& spec);

}  // namespace zetta::ddf
