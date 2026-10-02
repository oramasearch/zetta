#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::cluster { class ReplicationManager; }

namespace zetta::pipeline {

// events (RunStarted / RunSucceeded / RunFailed, plus a pre-start Pending):
//   Pending → Running → Succeeded | Failed.
enum class RunStatus {
    Pending,    // created, not yet started
    Running,    // executing (RunStarted)
    Succeeded,  // completed with outputs + lineage (RunSucceeded)
    Failed,     // failed; the logs surface the failing step + reason (RunFailed)
};

// The canonical lowercase wire name of a status, and the inverse. These are the
// exact spellings the `status` enum in run.hpp's JSON Schema accepts.
std::string_view to_string(RunStatus status);
std::optional<RunStatus> run_status_from_string(std::string_view name);

// One line in a run's log. Logs make a run observable + diagnosable (on failure,
// the failing step + reason live here).
struct RunLogEntry {
    std::string time;     // ISO-8601 UTC stamp
    std::string level;    // "info" / "warn" / "error" (free-form)
    std::string message;  // the log line
};

// One artifact a run produced. `kind` is one of "dataset" / "model" / "index".
struct RunOutput {
    // "dataset" | "model" | "index" | "report". A `report` names what a run
    // FOUND rather than what it produced — read it back with `get_run_report`
    // (#958); the ref is the run id, because the report is assembled at query
    // time from the run's model and its at-risk table rather than stored.
    std::string kind;
    std::string name;  // human label
    std::string ref;   // id / uri pointing at the artifact
};

// Where a run's results came from and how trustworthy they are.
struct RunLineage {
    std::vector<std::string> sources;  // source refs that fed the run
    std::vector<std::string> steps;    // step ids / names that ran
    std::string freshness;             // ISO-8601 stamp / freshness description
    double confidence = 0.0;           // 0..1 confidence score (clamped on write)
};

// The mutable, replicated/on-disk value of a run — everything the CRDT key
// `run:<uuid>` carries (the id is the key / folder name, so it isn't repeated).
// This is exactly the shape validated by the Run JSON Schema at every boundary.
struct RunValue {
    std::string project;                 // owning project uuid (optional)
    std::string graph;                   // executed graph id (optional, free)
    std::string project_name;            // denormalized display name (#1184)
    std::string pipeline_name;           // denormalized graph name (#1184)
    std::string deployment;              // owning deployment uuid (optional) —
                                         // set when a run is the execution of a
                                         // deployed graph (the reconcile loop ties
                                         // a run to its deployment through this)
    RunStatus status = RunStatus::Pending;
    std::string created;                 // ISO-8601 UTC stamp at creation
    std::string updated;                 // ISO-8601 UTC stamp; bumped on change
    std::string trace_id;                // the run's trace (32-hex W3C id,
                                         // optional) — every node's spans for
                                         // this run join it (#640)
    std::vector<RunLogEntry> logs;
    std::vector<RunOutput> outputs;
    RunLineage lineage;
    // Empty for integer entities. Text-backed runs persist the deterministic
    // derived-id map so their intermediate numeric keys remain auditable.
    nlohmann::json entity_key_mapping = nlohmann::json::object();
    // Frozen input choices, not a fresh analysis at report time (#1062).
    nlohmann::json features = {{"used", nlohmann::json::array()},
                               {"excluded", nlohmann::json::array()}};
    // Append-only training/evaluation measurements (#1137). Series are stored
    // column-wise as parallel steps/values/times arrays. The closed schema also
    // carries one-time flattened params, derived summaries and typed chart data.
    nlohmann::json metrics = {{"params", nlohmann::json::object()},
                              {"series", nlohmann::json::object()},
                              {"summary", nlohmann::json::object()},
                              {"artifacts", nlohmann::json::array()}};
    // Empty for ordinary runs; DP runs release only their existing final report.
    std::string metrics_suppressed;
};

// Serialize a run value to its JSON shape (no id). The inverse of
// parse_run_value; used for the CRDT value, run.json, and tests.
nlohmann::json to_json(const RunValue& value);

// The Run JSON Schema (draft-07) — the single source of truth for the value
// shape, exposed so callers/tests can introspect it.
const nlohmann::json& run_value_schema();
const nlohmann::json& run_features_schema();

// Boundary validation: check a JSON value against the Run JSON Schema. Returns
// true when valid; on failure returns false and (if `error` is non-null) fills
// it with a human-readable reason. This is the gate every stored/replicated run
// value passes through before it is trusted.
bool validate_run_value(const nlohmann::json& value, std::string* error = nullptr);

// Parse + validate a JSON value (from run.json on disk or a `run:<id>` CRDT
// value) into a RunValue. Returns nullopt when the value fails schema
// validation — so malformed data is rejected at the boundary with a clear
// reason (in `error`) instead of corrupting the in-memory tree.
std::optional<RunValue> parse_run_value(const nlohmann::json& value,
                                        std::string* error = nullptr);

// A Run record: one execution of a deployed graph (status / logs / outputs /
// lineage). A top-level entity with no parent, modeled like zetta::pipeline::Project —
// **replicated state** (flat key `run:<uuid>`) whose in-memory + on-disk
// (`<run-uuid>/run.json`) forms are a projection of the Automerge CRDT document.
//
// The id is the folder name and never changes; `project`/`graph`/`deployment`
// are fixed at creation, while their display names follow renames. The mutable
// fields (status / logs / outputs / lineage) change as a
// run progresses: the user-facing mutators capture into the CRDT (when
// replicated), bump `updated`, and rewrite run.json. Reconciliation uses
// apply_value(), which updates the projection from the document with NO capture
// (so there is no feedback loop). Lookups hand back shared_ptr handles, never
// raw pointers, exactly like Database / Project. See docs/pipeline/runs.md.
class Run {
public:
    Run(std::string id, RunValue value, std::filesystem::path dir,
        zetta::cluster::ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    const std::string& project() const { return value_.project; }
    const std::string& graph() const { return value_.graph; }
    const std::string& project_name() const { return value_.project_name; }
    const std::string& pipeline_name() const { return value_.pipeline_name; }
    const std::string& deployment() const { return value_.deployment; }
    RunStatus status() const { return value_.status; }
    const std::string& created() const { return value_.created; }
    const std::string& updated() const { return value_.updated; }
    const std::vector<RunLogEntry>& logs() const { return value_.logs; }
    const std::vector<RunOutput>& outputs() const { return value_.outputs; }
    const RunLineage& lineage() const { return value_.lineage; }
    const nlohmann::json& entity_key_mapping() const {
        return value_.entity_key_mapping;
    }
    const RunValue& value() const { return value_; }
    const std::filesystem::path& dir() const { return dir_; }

    // User-facing mutators (the "update" half of the slice): each bumps
    // `updated`, captures `run:<id>` into the CRDT (when replicated), then
    // rewrites run.json. `confidence` is clamped into [0, 1] on the way in.
    void set_status(RunStatus status);
    void add_log(RunLogEntry entry);
    void set_outputs(std::vector<RunOutput> outputs);
    void set_lineage(RunLineage lineage);
    void set_trace_id(std::string trace_id);  // the run's trace (#640)
    bool set_entity_key_mapping(nlohmann::json mapping,
                                std::string* error = nullptr);
    bool set_features(nlohmann::json features, std::string* error = nullptr);
    void suppress_metrics_for_privacy();

    // Refresh denormalized names after creation, startup backfill, or a rename.
    // This metadata maintenance preserves the execution timestamps.
    void set_display_names(std::string project_name,
                           std::string pipeline_name);

    // Strict caller metric writes. A metric's step must be greater than its
    // previous step, including caller-supplied system.* values. Values are finite.
    bool log_metrics(std::uint64_t step,
                     const std::map<std::string, double>& values,
                     std::string time = {}, std::string* error = nullptr);
    // Owner-thread-only batch write (#1205; docs/ml/run-metrics.md). Caller
    // values remain strict. Automatic system samples advance independently
    // past their persisted last step, atomically with the caller's metrics.
    //
    // `coalesce` is for a trainer streaming one batch per round (#1217): the
    // value changes in memory at once, but the replicated capture and run.json
    // are written at most once per kCaptureInterval. Every whole-value capture
    // is kept in the CRDT's history, so a capture per boosting round grew the
    // document quadratically and made each owner-thread save slower — reads
    // stalled behind it. A deferred capture is written by flush_capture (the
    // Node's tick, its stop, or any later immediate capture).
    bool log_metrics_with_system_sample(
        std::uint64_t natural_step,
        const std::map<std::string, double>& values,
        const std::map<std::string, double>& system_values,
        std::string time = {}, std::string* error = nullptr,
        bool coalesce = false);

    static constexpr std::chrono::milliseconds kCaptureInterval{1000};
    // True while a coalesced change is in memory but not yet captured.
    bool capture_pending() const { return capture_pending_; }
    // Write a pending capture when its interval has passed (or `force`).
    // Returns true when nothing remains pending.
    bool flush_capture(bool force = false);
    // Params are recorded once. Keys are already flattened dotted names and
    // values are scalar JSON values.
    bool log_params(nlohmann::json params, std::string* error = nullptr);
    // A restarted Study trial keeps params and clears partial metric outputs.
    void reset_metrics_for_retry();
    // Artifact is validated against the Run's typed artifact union.
    bool log_artifact(nlohmann::json artifact, std::string* error = nullptr);

    // Read selected series from `from_step`; max_points applies per metric and
    // uses LTTB while retaining the global min/max envelope.
    nlohmann::json get_metrics(const std::vector<std::string>& names = {},
                               std::uint64_t from_step = 0,
                               std::optional<std::size_t> max_points =
                                   std::nullopt) const;

    // Replace the mutable value to match a (replicated) document value, then
    // rewrite run.json — with NO capture (used by reconciliation).
    void apply_value(RunValue value);

    // Write this run's value (+ id) to dir/run.json.
    void persist_meta() const;

    // The replicated/on-disk value shape (no id). Equivalent to to_json(value()).
    nlohmann::json metadata() const;

private:
    void capture_and_persist();  // bump updated, capture (if any), rewrite disk

    std::string id_;
    RunValue value_;
    bool capture_pending_ = false;
    std::chrono::steady_clock::time_point last_capture_{};
    std::filesystem::path dir_;
    zetta::cluster::ReplicationManager* replication_;  // null = standalone (not replicated)
};

}  // namespace zetta::pipeline
