#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "zetta/governance/audit_event.hpp"

namespace zetta {

class ReplicationManager;

// The lifecycle state of a compute job (ARC-11 / ENT-31). The closed taxonomy of
// states a job moves through, grounded in the job events:
//   Queued → Running → Succeeded | Failed.
// Succeeded / Failed are terminal (a finished job is never restarted in place).
enum class JobStatus {
    Queued,     // created, accepted, not yet scheduled onto compute
    Running,    // executing on the scheduler / collectives
    Succeeded,  // completed; its outputs (run / dataset) are produced
    Failed,     // failed; `detail` surfaces the reason
};

// What kind of work a compute job performs — the unit of accelerated compute in
// the UC-1 churn pipeline. A closed enum; new compute kinds extend it.
enum class JobKind {
    Featurize,  // build features from source tables (point-in-time)
    Train,      // train a model
    Explain,    // explain a model (e.g. SHAP drivers)
    Score,      // batch-score entities against a model
    Ddf,        // a distributed dataframe (DDF) op (compute-near-data)
};

// The canonical lowercase wire names of a status / kind, and their inverses.
// These are the exact spellings the `status` / `kind` enums in the ComputeJob
// JSON Schema accept, and the strings stored in the CRDT value + on disk.
std::string_view to_string(JobStatus status);
std::optional<JobStatus> job_status_from_string(std::string_view name);
std::string_view to_string(JobKind kind);
std::optional<JobKind> job_kind_from_string(std::string_view name);

// True when a status is terminal (Succeeded / Failed) — i.e. a job is finished.
// The create/finish audit emission hinges on this transition.
bool is_terminal(JobStatus status);

// The mutable, replicated/on-disk value of a compute job — everything the CRDT
// key `job:<uuid>` carries (the id is the key / folder name, so it isn't repeated
// here). This is exactly the shape validated by the ComputeJob JSON Schema at
// every boundary (disk + the document). The `project` / `graph` / `run` /
// `scaling_policy` refs are optional plain ids (the zetta::ScalingPolicy reward
// model value type is built — see scaling_policy.hpp — but a job references it by
// id only; persisting policies as their own replicated entity is a later slice).
struct ComputeJobValue {
    JobKind kind = JobKind::Featurize;
    JobStatus status = JobStatus::Queued;
    std::string project;         // owning project uuid (optional)
    std::string graph;           // source graph id (optional)
    std::string run;             // produced run uuid (optional)
    std::string scaling_policy;  // ScalingPolicy (reward) id ref (optional)
    std::string created;         // ISO-8601 UTC stamp at creation
    std::string updated;         // ISO-8601 UTC stamp; bumped on change
    std::string detail;          // human-readable note / failure reason (optional)
};

// Serialize a compute-job value to its JSON shape (no id). The inverse of
// parse_compute_job_value; used for the CRDT value, job.json, and tests.
nlohmann::json to_json(const ComputeJobValue& value);

// The ComputeJob JSON Schema (draft-07) — the single source of truth for the
// value shape, exposed so callers/tests can introspect it. Mirrors
// schemas/compute_job.schema.json.
const nlohmann::json& compute_job_value_schema();

// Boundary validation: check a JSON value against the ComputeJob JSON Schema.
// Returns true when valid; on failure returns false and (if `error` is non-null)
// fills it with a human-readable reason. Every stored/replicated job value passes
// through here before it is trusted.
bool validate_compute_job_value(const nlohmann::json& value,
                                std::string* error = nullptr);

// Parse + validate a JSON value (from job.json on disk or a `job:<id>` CRDT
// value) into a ComputeJobValue. Returns nullopt when the value fails schema
// validation — so malformed data is rejected at the boundary with a clear reason
// (in `error`) instead of corrupting the in-memory tree.
std::optional<ComputeJobValue> parse_compute_job_value(
    const nlohmann::json& value, std::string* error = nullptr);

// A ComputeJob: one unit of native distributed accelerated compute (ARC-11 /
// ENT-31) — featurize / train / explain / score / a DDF op — with a lifecycle
// (Queued → Running → Succeeded | Failed). A top-level entity with no parent,
// modeled like zetta::Run: **replicated state** (flat key `job:<uuid>`) whose
// in-memory + on-disk (`<job-uuid>/job.json`) forms are a projection of the
// Automerge CRDT document.
//
// The id is the folder name and never changes; `kind` and the refs are fixed at
// creation. The mutable fields (status / detail) change as the job progresses:
// the user-facing mutator captures into the CRDT (when replicated), bumps
// `updated`, rewrites job.json, and — on the first transition into a terminal
// state — emits a `job_finished` audit event through the AuditSink (the create
// side is emitted by Node::create_job). Reconciliation uses apply_value(), which
// updates the projection from the document with NO capture and NO audit (a peer's
// action arrives already-audited as its own replicated `audit:` event). Lookups
// hand back shared_ptr handles, never raw pointers. See docs/compute/compute-jobs.md.
class ComputeJob {
public:
    ComputeJob(std::string id, ComputeJobValue value, std::filesystem::path dir,
               ReplicationManager* replication = nullptr,
               AuditSink* audit = nullptr);

    const std::string& id() const { return id_; }
    JobKind kind() const { return value_.kind; }
    JobStatus status() const { return value_.status; }
    const std::string& project() const { return value_.project; }
    const std::string& graph() const { return value_.graph; }
    const std::string& run() const { return value_.run; }
    const std::string& scaling_policy() const { return value_.scaling_policy; }
    const std::string& created() const { return value_.created; }
    const std::string& updated() const { return value_.updated; }
    const std::string& detail() const { return value_.detail; }
    const ComputeJobValue& value() const { return value_; }
    const std::filesystem::path& dir() const { return dir_; }

    // User-facing mutator (the "update" half of the slice): set the lifecycle
    // status, optionally recording a `detail` note (non-empty replaces; empty
    // leaves it). Bumps `updated`, captures `job:<id>` into the CRDT (when
    // replicated), then rewrites job.json. On the **first** transition into a
    // terminal state (Succeeded / Failed), emits a `job_finished` audit event
    // through the AuditSink (when one is attached) — once and only once.
    void set_status(JobStatus status, std::string detail = {});

    // Replace the mutable value to match a (replicated) document value, then
    // rewrite job.json — with NO capture and NO audit (used by reconciliation).
    void apply_value(ComputeJobValue value);

    // Write this job's value (+ id) to dir/job.json.
    void persist_meta() const;

    // The replicated/on-disk value shape (no id). Equivalent to to_json(value()).
    nlohmann::json metadata() const;

private:
    void capture_and_persist();  // bump updated, capture (if any), rewrite disk

    std::string id_;
    ComputeJobValue value_;
    std::filesystem::path dir_;
    ReplicationManager* replication_;  // null = standalone (not replicated)
    AuditSink* audit_;                 // null = not audited (no node behind it)
};

}  // namespace zetta
