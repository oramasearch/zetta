#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute_job.hpp"

// **Burst-job eviction tolerance — checkpoint / restart** (RULE-5 / RSK-9, #468).
//
// zetta bursts compute onto **spot / preemptible** cloud nodes (the Azure / AWS /
// GCP / OCI / k8s / bare-metal provisioners all request spot capacity), and a spot
// node can be **reclaimed mid-job** with only a short termination notice. The
// cross-node [work-stealing scheduler](../work-stealing-scheduler.md) already
// recovers a lost peer's in-flight *tasks* within a single `map()` call, but an
// iterative SPMD job (e.g. the many boosting rounds of distributed
// [GBDT training](../gbdt.md)) has no way to survive losing the node that drives
// it — today it restarts from scratch.
//
// This module is the missing **job-level** capability: a durable, fingerprinted
// **checkpoint** of a job's progress plus a driver that **resumes from the latest
// checkpoint** after a restart, so the rounds already completed are never
// recomputed. It is a standalone seam (no Node / SQL / cluster wiring of its own,
// like `Provisioner` / `DeviceRegistry`): the long-running job bodies and the
// restart trigger compose it. See docs/job-checkpoint.md.
namespace zetta::compute {

// ---- CheckpointStatus -------------------------------------------------------

// Why a checkpoint-store call did not succeed. A closed enum; the lowercase name
// is the stored/display spelling.
enum class CheckpointCode {
    Ok,               // the call completed
    NotFound,         // no checkpoint exists for the job
    Corrupt,          // a checkpoint was found but failed validation / its hash
    InvalidArgument,  // a precondition was violated (empty job id, bad value)
    IoError,          // the underlying store could not read / write
};

// The lowercase canonical name of a status code ("ok", "not_found", …).
constexpr std::string_view to_string(CheckpointCode code) {
    switch (code) {
        case CheckpointCode::Ok:              return "ok";
        case CheckpointCode::NotFound:        return "not_found";
        case CheckpointCode::Corrupt:         return "corrupt";
        case CheckpointCode::InvalidArgument: return "invalid_argument";
        case CheckpointCode::IoError:         return "io_error";
    }
    return "unknown";
}

// The outcome of a checkpoint-store call: a code plus, on failure, a reason.
// Non-throwing across the seam (zetta-core's convention) — the status IS the
// failure channel. A default-constructed status is Ok. Named constructors read
// clearer than aggregate init (the ComputeStatus / IndexStatus pattern).
struct CheckpointStatus {
    CheckpointCode code = CheckpointCode::Ok;
    std::string message;

    bool ok() const noexcept { return code == CheckpointCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }

    static CheckpointStatus success() { return {}; }
    static CheckpointStatus not_found(std::string reason) {
        return {CheckpointCode::NotFound, std::move(reason)};
    }
    static CheckpointStatus corrupt(std::string reason) {
        return {CheckpointCode::Corrupt, std::move(reason)};
    }
    static CheckpointStatus invalid_argument(std::string reason) {
        return {CheckpointCode::InvalidArgument, std::move(reason)};
    }
    static CheckpointStatus io_error(std::string reason) {
        return {CheckpointCode::IoError, std::move(reason)};
    }
};

// ---- JobCheckpoint (the durable progress snapshot) --------------------------

// A durable snapshot of a job's progress — everything needed to resume it without
// re-doing the work already committed. Boundary-validated against a formal JSON
// Schema (`job_checkpoint_schema()` / schemas/job_checkpoint.schema.json), like the
// Run / Model / ComputeJob records, **plus** a cross-field integrity check: the
// recorded `state_sha256` must equal the SHA-256 of the decoded `state` bytes, so a
// torn / corrupt checkpoint (an eviction mid-write) is rejected at the boundary
// instead of resuming from garbage — exactly the way a large-object body is verified
// before it is trusted.
struct JobCheckpoint {
    // The logical job key the checkpoint belongs to. For a single-node job this is
    // the ComputeJob id; for a distributed (SPMD) job it is the **cross-node-agreed
    // key** (the train job's `round_tag`), since each node holds a distinct
    // ComputeJob id but they must all resume from the same checkpoint.
    std::string job_id;
    JobKind kind = JobKind::Train;     // the kind of work (for observability)
    std::uint64_t sequence = 0;        // units of progress completed = the resume
                                       // point (e.g. boosting rounds / trees built)
    std::uint64_t total = 0;           // total units, when known; 0 = unknown
    std::string state;                 // opaque application bytes (the job's
                                       // resumable state; base64'd in JSON)
    std::string state_sha256;          // lowercase-hex SHA-256 of `state`
    std::string created;               // ISO-8601 UTC stamp when written

    // Fraction complete in [0, 1] — `sequence / total` (0 when total is 0).
    double progress() const;

    // Build a checkpoint, stamping `created` (now) and `state_sha256` (from
    // `state`). The single place those derived fields are computed.
    static JobCheckpoint make(std::string job_id, JobKind kind,
                              std::uint64_t sequence, std::uint64_t total,
                              std::string state);

    // The JSON value shape (state base64-encoded). Mirrors the schema.
    nlohmann::json to_json() const;
};

// The JobCheckpoint JSON Schema (draft-07) — the single source of truth for the
// value shape (schema-as-code; schemas/job_checkpoint.schema.json is generated from
// it and drift-checked by tests/test_schemas.cpp).
const nlohmann::json& job_checkpoint_schema();

// Boundary validation: check a JSON value against the JobCheckpoint JSON Schema.
// Returns true when the SHAPE is valid (the cross-field SHA-256 integrity check is
// done by parse_job_checkpoint, which decodes the body).
bool validate_job_checkpoint(const nlohmann::json& value,
                             std::string* error = nullptr);

// Parse + validate a JSON value into a JobCheckpoint: schema validation, then the
// `state_sha256` integrity check over the decoded body. Returns nullopt (with a
// reason in `error`) on a malformed shape or a hash mismatch — so corrupt data is
// rejected at the boundary instead of corrupting a resume.
std::optional<JobCheckpoint> parse_job_checkpoint(const nlohmann::json& value,
                                                  std::string* error = nullptr);

// ---- CheckpointStore (the durability seam) ----------------------------------

// Where a job's checkpoints live durably so they survive the node dying. The
// pluggable seam every backend implements; the long-running job bodies and the
// resume driver call it. Non-throwing — every call returns a CheckpointStatus.
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    // Durably record `checkpoint`. Replaces any earlier checkpoint for the same
    // job that is at or behind this `sequence` (a later checkpoint supersedes an
    // earlier one). Must be crash-safe: a failure / eviction mid-write must not
    // destroy the previous valid checkpoint.
    virtual CheckpointStatus save(const JobCheckpoint& checkpoint) = 0;

    // The latest valid checkpoint for `job_id` (highest `sequence`, schema-valid,
    // hash-verified), or nullopt when none exists / all are corrupt. On a non-Ok
    // outcome `status` (when non-null) carries the reason (NotFound is expected and
    // simply means "start fresh"). A read — `const` so a const holder can resume.
    virtual std::optional<JobCheckpoint> latest(
        const std::string& job_id, CheckpointStatus* status = nullptr) const = 0;

    // Discard every checkpoint for `job_id` (called once the job's outputs are
    // durable elsewhere, so a restart of a finished job does not resume). An
    // unknown job is a no-op success (idempotent).
    virtual CheckpointStatus clear(const std::string& job_id) = 0;

    // The job ids that currently have at least one valid checkpoint. The source for
    // an observability view (system.checkpoints). Order is unspecified.
    virtual std::vector<std::string> job_ids() const = 0;
};

// ---- FileCheckpointStore (the durable reference backend) --------------------

// A CheckpointStore backed by the local filesystem under a root directory — the
// durability that survives the process / node going away (a spot reclaim restarts
// the job on a surviving or replacement node, which reads the checkpoint back from
// durable storage). One folder per job (`<root>/<job-id-slug>/`), one file per
// checkpoint (`<sequence>.json`, zero-padded). `save` writes to a temp file and
// atomically renames it into place, so an interrupted write leaves the previous
// checkpoint intact; only the latest valid checkpoint (plus one prior, for
// torn-write safety) is retained.
//
// Note: a FileCheckpointStore is **local to one node's filesystem**. A single-node
// job (or a job whose driving node's process restarts in place) resumes directly.
// For a distributed job whose node is replaced, the surviving fleet must share /
// replicate the checkpoint storage — wiring this seam to a replicated store is a
// later slice; the seam is injected so that swap is transparent.
class FileCheckpointStore : public CheckpointStore {
public:
    explicit FileCheckpointStore(std::filesystem::path root);

    CheckpointStatus save(const JobCheckpoint& checkpoint) override;
    std::optional<JobCheckpoint> latest(
        const std::string& job_id,
        CheckpointStatus* status = nullptr) const override;
    CheckpointStatus clear(const std::string& job_id) override;
    std::vector<std::string> job_ids() const override;

    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
};

// ---- run_resumable_rounds (the checkpoint / restart driver) -----------------

// How to advance an iterative job by one round: given the round index (0-based)
// and the current accumulated state (opaque application bytes), return the new
// state after that round. Must not throw across the seam.
using RoundStep =
    std::function<std::string(std::uint64_t round, const std::string& state)>;

// An optional graceful-drain signal: return true to stop after the current round
// (the spot-eviction pattern — finish the round in flight, checkpoint, and exit so
// a restart resumes). Empty / always-false runs to completion.
using StopRequested = std::function<bool()>;

// Knobs for run_resumable_rounds.
struct ResumableConfig {
    std::uint32_t total_rounds = 0;     // rounds to run (the job's length)
    std::uint32_t checkpoint_every = 1;  // checkpoint after every N completed rounds
                                         // (>=1; the last round always checkpoints)
    bool clear_on_complete = true;       // discard checkpoints once all rounds done
};

// The outcome of a resumable run.
struct ResumableResult {
    bool completed = false;          // all total_rounds are done
    std::uint64_t resumed_from = 0;  // round we resumed at (0 = started fresh)
    std::uint64_t rounds_executed = 0;   // rounds actually run THIS invocation
    std::uint64_t rounds_completed = 0;  // rounds done overall (== total when done)
    std::string state;               // the final (or last-checkpointed) state
    CheckpointStatus status;         // Ok on success; an error if a save failed
};

// Drive an iterative job through its rounds with checkpoint / restart, the
// eviction-tolerant way:
//   1. Load the latest checkpoint for `job_id`; if one exists, resume from its
//      `sequence` with its `state` (the `initial_state` argument is then ignored).
//      Otherwise start at round 0 with `initial_state`.
//   2. Run each remaining round via `step`, checkpointing the new state every
//      `checkpoint_every` rounds (and always after the last).
//   3. Stop early — checkpointing first — when `stop_requested()` is true (a
//      graceful drain). On full completion, optionally `clear()` the checkpoints.
//
// Calling this again over the **same store** (a fresh process / node after an
// eviction) resumes where the last checkpoint left off, so the rounds already
// committed are never recomputed. Non-throwing: a store failure stops the run and
// is reported in the result's `status`.
ResumableResult run_resumable_rounds(const std::string& job_id, JobKind kind,
                                     CheckpointStore& store,
                                     const ResumableConfig& config,
                                     const std::string& initial_state,
                                     const RoundStep& step,
                                     const StopRequested& stop_requested = {});

}  // namespace zetta::compute
