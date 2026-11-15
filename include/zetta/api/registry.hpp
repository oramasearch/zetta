#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "zetta/api/dataset_scope.hpp"

#include "zetta/governance/capability.hpp"
#include "zetta/governance/consent.hpp"
#include "zetta/governance/safe_mode.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/runtime.hpp"

namespace zetta {
class Node;
}
namespace zetta::cluster {
class Cluster;
}
namespace zetta::pipeline {
class ProjectGraph;
}
namespace zetta::cloud {
class Provisioner;
}

// The capability registry — the single declaration point for the programmatic
// control surface (docs/api/registry-design.md, #614/#615). Each Capability is
// registered exactly once and rendered by thin layers: the MCP server lists +
// calls entries, the REST layer (#618) derives routes + OpenAPI from the same
// schemas, and the command palette (#625) is the same list rendered for humans.
//
// The registry itself is HTTP-free (zetta-core); the Beast2 server adapter
// (src/core/api/http_server.cpp) is its first consumer. Handlers are invoked
// on the loop that owns the Node (via MainThreadBridge — see bridge.hpp), are
// non-throwing, and return ApiResult — domain failures carry the engine's
// path-qualified reason verbatim, never a laundered "operation failed".
namespace zetta::api {

// The non-throwing call outcome. `error` codes follow the JSON-RPC mapping in
// registry-design.md: schema/protocol problems become protocol errors at the
// server layer; domain failures stay results with `ok == false` so the caller
// (an agent) can read the reason and adapt.
struct ApiError {
    std::string code;     // "invalid_input" | "unauthorized" | "not_found" |
                          // "engine_error" | "unknown_capability" | "busy" |
                          // "consent_required" | "conflict" (a name already
                          // taken, e.g. upload_dataset without if_exists rename) |
                          // "forbidden" (read-only by design: Orama's catalogue
                          // methodologies, zetta#1261) | "runner_required" (a
                          // workspace methodology's pipeline runs in Lake's runner;
                          // `details` says what to run, zetta#1261) |
                          // "rate_limited" (a published pipeline's per-caller
                          // limit, zetta#1289) | "payload_too_large" (its input
                          // size limit, zetta#1289) | "safe_mode_warning" (the
                          // database's safe mode warns on this statement;
                          // `details` names it and its confirmation, #1373)
    std::string message;  // the engine's reason, verbatim where one exists
    std::string path;     // JSON pointer / field path when the input is at fault
    // Structured payload riding with the failure — the consent challenge for
    // "consent_required" (#621): the call stays an error (nothing executed),
    // but the caller gets the full named request, not a laundered string.
    nlohmann::json details;
};

struct ApiResult {
    bool ok = false;
    nlohmann::json value;  // capability output on success (documented shape)
    ApiError error;        // populated when !ok

    // Observability, threaded back through every surface (the MCP response
    // tells the agent what happened and where to read more — get_trace).
    std::string trace_id;
    std::string span_id;
    double duration_ms = 0.0;

    static ApiResult success(nlohmann::json output) {
        ApiResult result;
        result.ok = true;
        result.value = std::move(output);
        return result;
    }
    static ApiResult failure(std::string code, std::string message,
                             std::string path = {}) {
        ApiResult result;
        result.ok = false;
        result.error = ApiError{std::move(code), std::move(message), std::move(path)};
        return result;
    }
};

// read: no state change. action: mutates and completes within a tick.
// job: submits work and returns the durable engine handle (a Run or ComputeJob
// id) immediately — long work is never awaited inside a call
// (registry-design.md → "no new job infrastructure").
enum class CapabilityKind { Read, Action, Job };

enum class AuditPolicy {
    PerCall,
    SummarizeAuthorizedReads,
    HandlerRecordsSuccess,
};

std::string_view to_string(CapabilityKind kind);

class BackgroundActions;

// Resolves a provision target id ("local", "azure", …) to the configured
// Provisioner the embedding wired for it — the consent-gated `add_nodes`
// capability's backend interface. Null function or null result = that target has
// no provisioner wired here (the handler fails with a clear reason; nothing
// is provisioned). The returned pointer is non-owning and must outlive the
// server (the same contract as App::set_provisioner).
using ProvisionerResolver =
    std::function<zetta::cloud::Provisioner*(const std::string& target_id)>;

class Registry;

// What a handler sees. `acting_key` is the acting identity the auth gate
// resolved: under the default-deny gate (#745) it is a USER id (see
// `acting_is_user`); otherwise it is the AccessKey id the bearer token mapped
// to, or empty for the audited "unauthenticated local" session (the
// governed-console precedent). The context is only ever constructed on the
// loop that owns the Node, so handlers may touch the node freely but must be
// quick (they run inside a tick).
struct ApiContext {
    Node& node;
    const zetta::cluster::Cluster* cluster = nullptr;  // for system.nodes
    std::string acting_key;                             // user/AccessKey id or empty

    // The trace this call belongs to (an inbound traceparent, or a fresh root
    // made by the server layer) and the node's span ring. Null recorder =
    // spans become no-ops (bare library use).
    zetta::observability::TraceContext trace;
    zetta::observability::SpanRecorder* recorder = nullptr;

    // Owner for the worker handles `kind: job` capabilities launch (#617,
    // background.hpp). Null = job capabilities fail with a clear reason (an
    // embedding that offers them must prune this each tick, beside the bridge
    // drain).
    BackgroundActions* background = nullptr;

    // The provisioner backends this embedding wired (#621) — consulted by the
    // consent-gated `add_nodes` capability. Empty = no targets wired here.
    ProvisionerResolver provisioners;

    // The default-deny gate (#745) resolved a bearer TOKEN to a user, so
    // `acting_key` is that user's id: authorize the operation against the
    // user's effective permissions (the union of its roles' capabilities),
    // not against an AccessKey. False = the legacy path (`acting_key` is an
    // AccessKey id, or empty for the unauthenticated-local session).
    bool acting_is_user = false;

    // Set by Registry::decide_consent while executing an approved challenge:
    // the challenge whose outcome the execution must land on. Handlers that
    // finish their work asynchronously (the cloud workers) read it so the
    // worker can record the outcome on the ledger at completion, and set
    // `consent_outcome_deferred` so decide_consent leaves the challenge in
    // its executing state instead of recording the immediate return value.
    std::string consent_challenge_id;
    bool consent_outcome_deferred = false;

    // The registry executing this call, set by Registry::call before it invokes
    // the handler. A handler that must run ANOTHER capability on the caller's
    // behalf — `chat`, which lets the agent read the node through an allowlist of
    // read-only capabilities (#904) — calls back through this, so the caller's
    // permissions and the audit trail apply to the inner call exactly as they
    // would to a direct one. Null outside a registry call.
    const Registry* registry = nullptr;

    // REST and MCP supply this one-shot event for an awaitable read. The handler
    // registers it before its no-wait read, so a publish between that read and
    // the route's co_await cannot be missed (#1076). In-process callers leave it
    // empty and receive the same immediate answer as wait_ms 0.
    std::shared_ptr<runtime::async_event> wait_event;

    // A published pipeline's pinned version (zetta#1289, docs/api/serving.md).
    // Set only by the served run path for its one inner `run`: `run` executes
    // this graph in place of the stored graph with the same id, so an edit made
    // after the pipeline was exposed is not what its callers get. Null
    // everywhere else.
    std::shared_ptr<zetta::pipeline::ProjectGraph> served_graph;

    // The per-call dataset scope (#1515, dataset_scope.hpp): set by REST from
    // the `Zetta-Dataset-Scope` header. When set, Registry::call runs the
    // capability only if it declares its dataset reads for this input and every
    // one is in the scope. Unset = not confined.
    std::optional<DatasetScope> dataset_scope;
};

using CapabilityHandler =
    std::function<ApiResult(const nlohmann::json& input, ApiContext& context)>;

// Names every dataset a capability reads for this (already schema-validated)
// input (#1515). nullopt = this input reads something that is not a dataset, or
// what it reads cannot be told before it runs: under a dataset scope the call is
// refused. Consulted only for a dataset-scoped call.
using DatasetReadsDescriber = std::function<std::optional<std::vector<DatasetName>>(
    const nlohmann::json& input)>;

// Names exactly what a consent-gated capability WOULD read/run for this
// (already schema-validated) input — the Cloud Inspector grammar the returned
// challenge carries (#621). Returning std::nullopt means THIS input touches no
// cloud (e.g. `add_nodes` on the `local` target) and the call may execute
// without a challenge — the same input-aware gate the headed GUI applies.
using ConsentDescriber = std::function<std::optional<governance::ConsentAction>(
    const nlohmann::json& input, ApiContext& context)>;

// Names the statements a safe-mode-guarded capability is about to run for this
// (already schema-validated) input, one entry per database they run against
// (docs/api/safe-mode.md). Empty = nothing this input runs is guarded (a parse
// error the handler reports, an inline connector with no saved name, a
// statement the source's read-only guard refuses anyway). Runs on the owner
// thread.
using SafeModeDescriber = std::function<std::vector<governance::SafeModeStatements>(
    const nlohmann::json& input, ApiContext& context)>;

// The input field that carries a safe-mode confirmation. Registry::add adds it
// to the input schema of every capability with a SafeModeDescriber, and
// Registry::call removes it before the handler runs.
inline constexpr const char* kSafeModeConfirmationField = "safe_mode_confirmation";

// REST-only execution policy for a registry read that may suspend without
// holding a thread. Registry::call remains the one governed call path: the
// route calls it once for the initial no-wait read and, only when should_wait
// says the result is empty, once after the event or timeout (#1076).
struct AwaitableRead {
    std::function<std::chrono::milliseconds(const nlohmann::json& input)> wait_for;
    std::function<bool(const ApiResult& result)> should_wait;
    // Optional (#1005): a call whose answer changes with time as well as with
    // the event. When set, the route keeps waiting until wait_for elapses — each
    // wake (the event, or retry_within(result) passing) makes one more governed
    // call with a fresh event — instead of making one final call. Such an entry
    // may be an action that mutates and blocks (acquire_object_lease waits out
    // another holder's lease this way); offloadable and consent-gated ones may not.
    std::function<std::chrono::milliseconds(const ApiResult& result)> retry_within;
};

// One entry on the control surface. `input_schema` is draft-07 JSON Schema
// (schema-as-code — declared next to the handler, validated at the boundary
// with the same validator discipline as the Run record). `output_schema`
// documents the success value's shape (draft-07 too): it renders as the MCP
// tool's outputSchema and as the REST/OpenAPI 200 response (#618) — asserted
// in journeys/tests, not enforced at runtime. `description` must be one
// agent-actionable sentence that states side effects.
struct Capability {
    std::string name;         // snake_case: verb-first actions, noun reads
    std::string title;
    std::string description;
    nlohmann::json input_schema;
    nlohmann::json output_schema;
    CapabilityKind kind = CapabilityKind::Read;
    zetta::governance::Operation required_operation;  // api:query / orchestration:<name>
    bool mutates = false;           // fires the activity hook on success
    bool requires_consent = false;  // cloud-touching: consent challenge (#621)
    CapabilityHandler handler;
    // Consulted only when requires_consent: names what this input would
    // read/run (the challenge body). Missing describer = a generic challenge
    // built from `description`. See ConsentDescriber above.
    ConsentDescriber describe_consent;
    // Safe mode per database (#1373): when set, Registry::call asks it what
    // this input would run and refuses with `safe_mode_warning` when the
    // database's level warns on it, until the call is repeated with the
    // warning's confirmation. See SafeModeDescriber above.
    SafeModeDescriber describe_safe_mode;
    // What this capability reads for an input, for a dataset-scoped call (#1515).
    // Missing = it cannot run under a dataset scope. See DatasetReadsDescriber.
    DatasetReadsDescriber dataset_reads;
    // Run this capability off the owner/tick thread, on a pool worker
    // (runtime::offload), instead of marshaling it onto the owning loop. ONLY safe
    // for a handler that touches no main-thread-only Node state — just the
    // thread-safe ReplicationManager + pure compute (e.g. chat: model inference).
    // Keeps a long CPU/GPU decode from pinning the tick loop (#731). Registry::call
    // still runs its authorization + audit gate on the owner thread (#1063), and
    // add() refuses an offloadable entry that mutates or requires consent.
    bool offloadable = false;
    // An empty REST or MCP read may suspend on a runtime event, bounded by wait_for,
    // then make one final governed call. No thread is held while it waits.
    std::optional<AwaitableRead> awaitable_read;
    // Also accept a JSON-body POST for a read, retaining its GET route and read classification.
    bool post_alias = false;
    // Object-tier fallback when there is no global operation grant. Runs on
    // the owner thread after the per-tool gate; handlers still check each
    // addressed object, including every item in a filtered listing.
    std::function<bool(const nlohmann::json&, const ApiContext&)> authorize_scoped;
    // High-frequency successful reads may be summarized by actor, capability,
    // and one input scope. Denials and failures remain per-call (#1154).
    AuditPolicy audit_policy = AuditPolicy::PerCall;
    std::string audit_scope_field;
    // The output field that carries accessed data. An empty array/object/string
    // means an idle read; a present scalar means data was returned. This keeps
    // empty polling windows out of the audit log without hiding successful
    // reads that returned topic metadata (#1169).
    std::string audit_result_field;
    // The handler performs blocking storage work against internally guarded
    // state. REST and MCP run it through runtime::offload_blocking after the
    // registry has completed its owner-thread authorization gate.
    bool blocks = false;
    // Appended to an input-schema rejection when it returns non-empty text:
    // the capability's one-step way past the error for this input (e.g.
    // create_study offers from_defaults, #1217). Empty = the reason alone.
    std::function<std::string(const nlohmann::json& input)> invalid_input_hint;
};

// The refusal a dataset-scoped call of `capability` with `input` gets (#1515),
// or nullopt when every dataset it reads is in `scope`. Registry::call applies
// it; exposed for tests and for an embedding that dispatches by hand.
std::optional<ApiResult> dataset_scope_refusal(const Capability& capability,
                                               const nlohmann::json& input,
                                               const DatasetScope& scope);

// Fired after any successful `mutates` call so an embedding GUI can refresh
// immediately and record the action (#620/#622). Invoked on the owning loop.
struct ActivityEvent {
    std::string capability;  // the entry's name
    std::string actor;       // acting key id, or "local"
};
using ActivityHook = std::function<void(const ActivityEvent&)>;

// The registry. Not thread-safe by design: it is populated at startup and
// then only read; calls arrive on the owning loop via the bridge.
class Registry {
public:
    // Registers an entry. Returns false (and registers nothing) on a
    // duplicate name or a missing handler.
    bool add(Capability capability);

    std::shared_ptr<const Capability> find(const std::string& name) const;
    std::vector<std::shared_ptr<const Capability>> list() const;
    std::size_t size() const { return entries_.size(); }

    void set_activity_hook(ActivityHook hook) { activity_ = std::move(hook); }

    // The one call path every rendering uses: resolve -> validate the input
    // against the entry's schema -> authorize (capability check; every call is
    // audited as a data_access event with the decision) -> invoke the handler
    // -> fire the activity hook when a mutating call succeeds. Non-throwing.
    //
    // A `requires_consent` capability NEVER executes here (#621): when its
    // describer says the input touches a cloud, the call issues a one-time
    // consent challenge on the node's ledger (audited, span'd) and fails with
    // code "consent_required" carrying the challenge as structured details.
    // Approval goes through decide_consent below.
    //
    // A capability with a SafeModeDescriber is checked before consent: when the
    // level of a database it would run against warns on the statement, the call
    // fails with code "safe_mode_warning" naming the database, the level, the
    // exact statements and a confirmation; the same call repeated with
    // `safe_mode_confirmation` set to that value runs (audited as
    // safe_mode_confirmed).
    ApiResult call(const std::string& name, const nlohmann::json& input,
                   ApiContext& context) const;

    // Where a consent decision came from. The headed app's approval surface is
    // the trusted human-in-front-of-the-machine path (the GUI consent-gate
    // precedent); everything arriving over the wire is ApproverKey and must be
    // a NAMED key of a DIFFERENT principal than the requester — the calling
    // key (or the unauthenticated local session) can never approve its own
    // cloud action. This parameter is set by trusted in-process callers, never
    // from request input.
    enum class ConsentSurface { ApproverKey, HeadedApp };

    // Decide one pending challenge (#621). Approval consumes the challenge
    // (exactly once), re-authorizes the original requester, executes the
    // FROZEN capability input under the requester's identity, fires the
    // activity hook, and lands the outcome on the ledger — audited on both
    // sides (the decision under the approver, the action under the
    // requester). Denial records the refusal; the action never runs. Both
    // are spans. `context` is the DECIDER's context (its acting_key is the
    // approver when surface == ApproverKey).
    ApiResult decide_consent(const std::string& challenge_id, bool approve,
                             const std::string& reason, ApiContext& context,
                             ConsentSurface surface) const;

private:
    std::vector<std::shared_ptr<const Capability>> entries_;
    ActivityHook activity_;
};

}  // namespace zetta::api
