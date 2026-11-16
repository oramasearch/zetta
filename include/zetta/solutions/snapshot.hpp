// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Point-in-time snapshot tables, row-level as-of joins, and S-1 forward
// outcome labels (#1103, #1214).
//
// Snapshot tables use the logical key `(entity_id, as_of)`. Each materialized
// row also records `_source_as_of` and `_source_table`, so downstream lineage
// can prove which observation was visible at the requested cutoff. Tables are
// ordinary Node-owned tables: their metadata and rows follow the existing CRDT
// capture/reconcile path rather than introducing another state channel.
//
// See docs/solutions/point-in-time-snapshots.md.
namespace zetta {
class Node;
}

namespace zetta::data {
class Table;
}

namespace zetta::solutions {

enum class TimePointKind {
    Numeric,
    Iso8601,
};

struct TimePoint {
    TimePointKind kind = TimePointKind::Numeric;
    double value = 0.0;

    bool operator==(const TimePoint&) const = default;
};

// Parse a finite numeric instant or an ISO-8601 UTC date/date-time. A date-only
// value means midnight UTC. `error` receives a path-ready reason on failure.
std::optional<TimePoint> parse_time_point(const nlohmann::json& value,
                                          std::string* error = nullptr);

struct SnapshotSpec {
    std::string database;
    std::string source_table;
    std::string target_table;
    std::string entity_column;
    std::string time_column;
    nlohmann::json as_of;

    nlohmann::json to_json() const;
    static std::optional<SnapshotSpec> from_json(const nlohmann::json& value,
                                                 std::string* error = nullptr);

    bool operator==(const SnapshotSpec&) const = default;
};

const nlohmann::json& snapshot_spec_schema();

struct SnapshotResult {
    std::shared_ptr<zetta::data::Table> table;
    std::size_t source_rows = 0;
    std::size_t materialized_rows = 0;
    std::size_t existing_rows = 0;
    std::size_t future_rows_excluded = 0;
    std::string error;

    bool ok() const noexcept {
        return error.empty() && static_cast<bool>(table);
    }
};

// Materialize the latest source observation per entity at or before `as_of`.
// Repeating the same request is idempotent; a conflicting row at an existing
// `(entity_id, as_of)` key is refused. Partition-only sources are read through
// the existing one-owner-per-partition scatter/gather path.
SnapshotResult materialize_snapshot(Node& node, const SnapshotSpec& spec);

// S-1 learns whether an active entity has a positive outcome after the
// prediction cutoff, not whether it was already positive at that cutoff.
// Each retained training row carries features from at or before its cutoff and
// a label from (cutoff + blackout, cutoff + blackout + window]. A positive
// outcome completes observation immediately; a negative row is retained only
// when that entity has an observation at or beyond the window end.
struct ForwardLabelSpec {
    std::string label_column;
    std::vector<std::string> feature_columns;
    double blackout_gap_seconds = 0.0;
    double window_seconds = 0.0;
    std::size_t rolling_windows = 3;
};

struct ForwardLabelWindow {
    double cutoff = 0.0;
    std::size_t rows = 0;
    std::size_t positives = 0;
    std::size_t negatives = 0;
    std::size_t censored = 0;
    std::size_t inactive = 0;

    bool has_both_classes() const noexcept {
        return positives > 0 && negatives > 0;
    }
};

struct ForwardLabelResult {
    std::vector<nlohmann::json> rows;
    std::vector<ForwardLabelWindow> windows;
    double data_last_time = 0.0;
    double declared_window_end = 0.0;
    bool declared_window_fully_observed = false;
    std::size_t windows_with_both_classes = 0;
    std::size_t training_rows = 0;
    std::size_t training_positives = 0;
    std::size_t training_negatives = 0;
    std::size_t validation_rows = 0;
    std::size_t validation_positives = 0;
    std::size_t validation_negatives = 0;
    std::size_t censored = 0;
    std::size_t inactive = 0;
    std::string error;

    bool ok() const noexcept {
        return error.empty();
    }
};

// Read the snapshot source through its ordinary local or distributed table
// path and build rolling forward-labelled rows. The generated rows use the
// internal `_s1_sample_id` entity and `_s1_cutoff` time columns so repeated
// observations of one business entity remain distinct training examples.
ForwardLabelResult build_forward_labels(Node& node,
                                        const SnapshotSpec& snapshot,
                                        const ForwardLabelSpec& spec);

struct AsOfJoinSpec {
    std::string left_entity_column;
    std::string left_time_column;
    std::string right_entity_column = "entity_id";
    std::string right_time_column = "as_of";
    std::string right_prefix = "snapshot_";

    nlohmann::json to_json() const;
    static std::optional<AsOfJoinSpec> from_json(const nlohmann::json& value,
                                                 std::string* error = nullptr);

    bool operator==(const AsOfJoinSpec&) const = default;
};

const nlohmann::json& as_of_join_spec_schema();

struct AsOfJoinResult {
    std::vector<nlohmann::json> rows;
    std::size_t matched = 0;
    std::size_t unmatched = 0;
    std::size_t future_rows_excluded = 0;
    std::string error;

    bool ok() const noexcept {
        return error.empty();
    }
};

// For each left row, attach the latest right row for the same entity whose
// right time is <= the left row's decision time. Right payload fields are
// prefixed; key fields are reported as `<prefix>entity_id` and `<prefix>as_of`.
AsOfJoinResult as_of_join(const std::vector<nlohmann::json>& left_rows,
                          const std::vector<nlohmann::json>& right_rows,
                          const AsOfJoinSpec& spec);

}  // namespace zetta::solutions
