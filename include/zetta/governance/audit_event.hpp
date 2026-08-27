#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::governance {

// The governed action an audit event records (ENT-23). The closed taxonomy of
// auditable actions in this slice of the audit trail (FEAT-9): graph changes,
// deploys, and data-access decisions. New auditable actions extend this enum.
enum class AuditAction {
    GraphCreated,   // a graph was added under a project   (graph change)
    GraphDropped,   // a graph was removed from a project  (graph change)
    GraphDeployed,  // a graph was placed onto a target    (deploy)
    GraphApproved,  // a proposed plan was approved for build (#885, review-before-build)
    DataAccess,     // a request was authorized against a key's scope (data access)
    JobCreated,     // a compute job was queued            (compute, ARC-11)
    JobFinished,    // a compute job reached a terminal state (Succeeded / Failed)
    KeyIssued,      // an access key was issued (e.g. a burst node's identity, ARC-13)
    KeyRevoked,     // an access key was revoked (e.g. on burst teardown)
    ProvisionRequested,  // a provision burst hit the cost-cap approval gate (APR-3)
    CloudInspected,  // a consent-approved cloud inspect ran (provider CLI, #624)
    DatasetAdded,    // a dataset file was added as a partitioned table (#624)
    ConsentRequested,  // a remote consent challenge was issued (#621)
    ConsentDecided,    // a consent challenge was approved/denied (#621)
    ConsentBypassed,   // a consent-gated call ran ungated: enforcement is off (#995)
};

// The canonical lowercase wire name of an action, and the inverse. These are the
// exact spellings the `action` enum in the AuditEvent JSON Schema accepts, and
// the strings stored in the CRDT value + on-disk projection.
std::string_view to_string(AuditAction action);
std::optional<AuditAction> audit_action_from(std::string_view name);

// The immutable value of an audit event — everything the CRDT key `audit:<uuid>`
// carries (the id is the key / on-disk file name, so it isn't repeated here).
// This is exactly the shape validated by the AuditEvent JSON Schema at every
// boundary (disk + the document).
struct AuditEventValue {
    AuditAction action = AuditAction::DataAccess;
    std::string actor;    // who acted: the login key id / principal (optional)
    std::string target;   // what was acted on: graph name, deployment id, resource
    bool allowed = true;  // the outcome (data_access: the decision; true for changes)
    std::string detail;   // human-readable reason / summary (e.g. the deny reason)
    std::string time;     // ISO-8601 UTC stamp at the moment recorded
};

// Serialize an audit-event value to its JSON shape (no id). The inverse of
// parse_audit_event_value; used for the CRDT value, audit/<uuid>.json, and tests.
nlohmann::json to_json(const AuditEventValue& value);

// The AuditEvent JSON Schema (draft-07) — the single source of truth for the
// value shape, exposed so callers/tests can introspect it. Mirrors
// schemas/audit_event.schema.json.
const nlohmann::json& audit_event_value_schema();

// Boundary validation: check a JSON value against the AuditEvent JSON Schema.
// Returns true when valid; on failure returns false and (if `error` is non-null)
// fills it with a human-readable reason. Every stored/replicated audit value
// passes through here before it is trusted.
bool validate_audit_event_value(const nlohmann::json& value,
                                std::string* error = nullptr);

// Parse + validate a JSON value (from audit/<uuid>.json on disk or an
// `audit:<id>` CRDT value) into an AuditEventValue. Returns nullopt when the
// value fails schema validation — so malformed data is rejected at the boundary
// with a clear reason (in `error`) instead of corrupting the in-memory trail.
std::optional<AuditEventValue> parse_audit_event_value(
    const nlohmann::json& value, std::string* error = nullptr);

// A seam for appending audit events, decoupling the entities that *emit* them
// (e.g. a Project's graph CRUD) from the Node that *owns* the append-only trail.
// `Node` is the implementation; a Project holds a non-owning AuditSink* the same
// way it holds a ReplicationManager*. A null sink means "not audited" (the entity
// records nothing — e.g. a Project constructed without a node behind it).
class AuditSink {
public:
    virtual ~AuditSink() = default;

    // Append one immutable audit event. The sink stamps id + time, persists it,
    // and (when replicated) captures it into the document. Append-only: an event,
    // once recorded, is never edited or removed.
    virtual void record_audit_event(AuditEventValue value) = 0;
};

// An immutable, append-only audit record (ENT-23 / FEAT-9). One event captures a
// single governed action — a graph change, a deploy, or a data-access decision —
// with who did it, what it touched, the outcome, and when.
//
// An AuditEvent is **replicated state** like the rest of zetta: the Automerge
// CRDT document is the source of truth (flat key `audit:<uuid>`), and an
// AuditEvent is its in-memory + on-disk projection. Unlike the mutable entities,
// an audit event is **immutable and append-only**: there is no mutator and no
// drop path — events only ever accumulate, so the CRDT converges trivially (the
// set of `audit:` keys only grows). On disk it projects to a single file in a
// shared `audit/` folder (`<data_dir>/audit/<uuid>.json`), the natural shape for
// an append-only log, rather than a folder per event.
//
// The id is the file name / CRDT key and never changes. `metadata()` is the
// value shape that rides in the document and (with the id added) on disk.
class AuditEvent {
public:
    // `dir` is the shared audit directory (`<data_dir>/audit`); this event's file
    // is `dir / (id + ".json")`.
    AuditEvent(std::string id, AuditEventValue value,
               std::filesystem::path dir);

    const std::string& id() const { return id_; }
    AuditAction action() const { return value_.action; }
    const std::string& actor() const { return value_.actor; }
    const std::string& target() const { return value_.target; }
    bool allowed() const { return value_.allowed; }
    const std::string& detail() const { return value_.detail; }
    const std::string& time() const { return value_.time; }
    const AuditEventValue& value() const { return value_; }

    // The shared audit directory this event lives in, and its own file within it.
    const std::filesystem::path& dir() const { return dir_; }
    std::filesystem::path file() const { return dir_ / (id_ + ".json"); }

    // Write this event's value (+ id) to its file().
    void persist_meta() const;

    // The replicated/on-disk value shape (no id). Equivalent to to_json(value()).
    nlohmann::json metadata() const;

private:
    std::string id_;
    AuditEventValue value_;
    std::filesystem::path dir_;  // the shared audit/ directory
};

}  // namespace zetta::governance