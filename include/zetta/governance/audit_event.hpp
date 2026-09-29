#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    DataAccessSummary,  // windowed authorised polling reads (#1154)
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
    DecisionPolicyCreated,  // a versioned decision policy was created (#1104)
    DecisionPolicyUpdated,  // a new immutable policy version was created (#1104)
    DecisionApproved,       // a pending A1 decision was approved (#1104)
    PubSubTopicUpdated,
    PubSubEventUpdated,
    PubSubEventDeleted,
};

// Completeness metadata for an authorised high-frequency read window (#1154).
// The first read still records a session-start event; subsequent successful
// reads are counted here, including how many pub/sub events they returned.
struct AuditReadSummary {
    std::uint64_t calls = 0;
    std::uint64_t events_delivered = 0;
    std::string scope;
    std::string window_started;
    std::string window_ended;
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
    std::optional<AuditReadSummary> summary;
};

// Serialize an audit-event value to its JSON shape (no id). The inverse of
// parse_audit_event_value; used for the CRDT value, segment records, and tests.
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

// Parse + validate a JSON value (from a segment/legacy file on disk or an
// `audit:<id>` CRDT value) into an AuditEventValue. Returns nullopt when the
// value fails schema validation — so malformed data is rejected at the boundary
// with a clear reason (in `error`) instead of corrupting the in-memory trail.
std::optional<AuditEventValue> parse_audit_event_value(
    const nlohmann::json& value, std::string* error = nullptr);

// An interface for appending audit events, decoupling the entities that *emit* them
// (e.g. a Project's graph CRUD) from the Node that owns the retained trail.
// `Node` is the implementation; a Project holds a non-owning AuditSink* the same
// way it holds a ReplicationManager*. A null sink means "not audited" (the entity
// records nothing — e.g. a Project constructed without a node behind it).
class AuditSink {
public:
    virtual ~AuditSink() = default;

    // Append one immutable audit event. The sink stamps id + time, persists it,
    // and (when replicated) captures it into the document. An event is never
    // edited; node retention may expire an old record.
    virtual void record_audit_event(AuditEventValue value) = 0;
};

// An immutable audit record (ENT-23 / FEAT-9). One event captures a
// single governed action — a graph change, a deploy, or a data-access decision —
// with who did it, what it touched, the outcome, and when.
//
// An AuditEvent is **replicated state** like the rest of zetta: the Automerge
// CRDT document is the source of truth (flat key `audit:<uuid>`), and an
// AuditEvent is its in-memory + on-disk projection. Unlike mutable entities, an
// audit event cannot be edited. Retention can expire an entire old record while
// preserving summary records. On disk events project into checksummed gzip
// segments in the shared `audit/` folder.
//
// The id is the file name / CRDT key and never changes. `metadata()` is the
// value shape that rides in the document and (with the id added) on disk.
class AuditEvent {
public:
    // `dir` is the shared audit directory (`<data_dir>/audit`).
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

    // The shared audit directory and legacy per-event file path.
    const std::filesystem::path& dir() const { return dir_; }
    std::filesystem::path file() const { return dir_ / (id_ + ".json"); }

    // Write the legacy per-event representation, used by compatibility tests.
    void persist_meta() const;

    // The replicated/on-disk value shape (no id). Equivalent to to_json(value()).
    nlohmann::json metadata() const;

private:
    std::string id_;
    AuditEventValue value_;
    std::filesystem::path dir_;  // the shared audit/ directory
};

// The local projection of the replicated audit trail (#1154). Events are
// appended to checksummed gzip segments, while RAM holds only a compact id
// index and a bounded recent window. Legacy audit/<uuid>.json files are
// migrated when the store opens. Older records are inflated only for a read.
class AuditTrail {
public:
    struct MemoryUsage {
        std::uint64_t bytes = 0;
        std::uint64_t items = 0;
        std::uint64_t index_bytes = 0;
        std::uint64_t recent_bytes = 0;
        std::uint64_t pending_persist_bytes = 0;
        std::uint64_t pending_persist_items = 0;
    };

    using EventVisitor =
        std::function<void(const std::shared_ptr<AuditEvent>& event)>;
    using RemovedVisitor = std::function<void(const std::string& id)>;

    explicit AuditTrail(std::filesystem::path directory,
                        std::size_t recent_limit = 2'048);
    ~AuditTrail();

    AuditTrail(const AuditTrail&) = delete;
    AuditTrail& operator=(const AuditTrail&) = delete;

    bool open(std::string* error = nullptr);
    bool append(std::string id, AuditEventValue value,
                std::string* error = nullptr);
    bool contains(std::string_view id) const;
    std::size_t size() const;
    MemoryUsage memory_usage() const;

    std::vector<std::shared_ptr<AuditEvent>> events() const;
    std::vector<std::shared_ptr<AuditEvent>> recent_events() const;
    void for_each(const EventVisitor& visitor) const;

    // Remove ordinary records older than cutoff while retaining every summary.
    // Replacement segments are written and synced before directory renames
    // install them. removed is invoked once for each discarded id.
    bool retain_since(std::string_view cutoff, const RemovedVisitor& removed,
                      std::string* error = nullptr);
    void flush();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zetta::governance
