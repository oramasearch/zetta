#include "zetta/api/registry.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace zetta::api {

std::string_view to_string(CapabilityKind kind) {
    switch (kind) {
        case CapabilityKind::Read: return "read";
        case CapabilityKind::Action: return "action";
        case CapabilityKind::Job: return "job";
    }
    return "read";
}

bool Registry::add(Capability capability) {
    if (capability.name.empty() || !capability.handler) return false;
    if (find(capability.name)) return false;
    // An offloadable handler runs on a pool worker; the consent path and the
    // activity hook touch owner-thread state after the gate, so neither may
    // follow one (#1063).
    if (capability.offloadable && (capability.requires_consent || capability.mutates)) {
        return false;
    }
    // A retrying awaitable (#1005) may be a blocking, mutating action: each of
    // its calls is a whole governed call, made on the pool.
    const bool retrying = capability.awaitable_read && capability.awaitable_read->retry_within;
    if (capability.blocks &&
        (capability.offloadable || (capability.awaitable_read && !retrying) ||
         capability.requires_consent)) {
        return false;
    }
    if (capability.awaitable_read &&
        (capability.offloadable || capability.requires_consent ||
         !capability.awaitable_read->wait_for || !capability.awaitable_read->should_wait ||
         (!retrying && (capability.kind != CapabilityKind::Read || capability.mutates)))) {
        return false;
    }
    if (capability.audit_policy == AuditPolicy::SummarizeAuthorizedReads &&
        (capability.kind != CapabilityKind::Read || capability.mutates ||
         capability.requires_consent)) {
        return false;
    }
    if (capability.audit_policy == AuditPolicy::HandlerRecordsSuccess &&
        (capability.kind != CapabilityKind::Action || !capability.mutates)) {
        return false;
    }
    entries_.push_back(std::make_shared<const Capability>(std::move(capability)));
    return true;
}

std::shared_ptr<const Capability> Registry::find(const std::string& name) const {
    auto found = std::find_if(entries_.begin(), entries_.end(),
                              [&](const auto& entry) { return entry->name == name; });
    return found == entries_.end() ? nullptr : *found;
}

std::vector<std::shared_ptr<const Capability>> Registry::list() const {
    return entries_;
}

namespace {

// Validate `input` against the entry's draft-07 schema. Violations are
// collected through the validator's error callback — never thrown across
// the library boundary (the run.cpp pattern; zetta-core does not unwind).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json&, const std::string& message) override {
        if (!failed_) {
            failed_ = true;
            reason_ = pointer.to_string().empty()
                          ? message
                          : pointer.to_string() + ": " + message;
            path_ = pointer.to_string();
            // A missing required property is reported at its parent; the
            // field at fault is the property itself (#1217).
            constexpr std::string_view kRequired = "required property '";
            // So is a property the schema does not allow (#1220): the
            // validator reports it at the object that holds it.
            constexpr std::string_view kAdditional =
                "validation failed for additional property '";
            for (const std::string_view prefix : {kRequired, kAdditional}) {
                if (!message.starts_with(prefix)) continue;
                const std::size_t end = message.find('\'', prefix.size());
                if (end != std::string::npos) {
                    path_ += "/" + message.substr(prefix.size(),
                                                  end - prefix.size());
                }
            }
        }
    }
    bool failed() const { return failed_; }
    const std::string& reason() const { return reason_; }
    const std::string& path() const { return path_; }

private:
    bool failed_ = false;
    std::string reason_;
    std::string path_;
};

// `contentEncoding` (#1231): base64 is checked; any other encoding is refused,
// so a schema never claims a check it does not get. A throw here is collected by
// the validator as an error at the field.
void check_content(const std::string& encoding, const std::string& /*media_type*/,
                   const nlohmann::json& instance) {
    if (encoding.empty()) return;
    if (encoding != "base64") {
        throw std::invalid_argument("contentEncoding '" + encoding + "' is not supported");
    }
    if (!instance.is_string()) throw std::invalid_argument("want a base64 string");
    const std::string& text = instance.get_ref<const std::string&>();
    std::size_t padding = 0;
    bool valid = text.size() % 4 == 0;
    for (const char character : text) {
        if (!valid) break;
        if (character == '=') {
            ++padding;
            continue;
        }
        valid = padding == 0 &&
                ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                 (character >= '0' && character <= '9') || character == '+' || character == '/');
    }
    if (!valid || padding > 2) throw std::invalid_argument("not valid base64");
}

// Validates `input`; on failure fills the reason and the JSON pointer of the
// field at fault, so a schema rejection carries `path` like a handler's own.
// Never throws: a schema the validator cannot compile (an unsupported keyword, a
// bad pattern) is an invalid_input refusal naming the problem, not an exception
// escaping Registry::call.
bool validate_input(const nlohmann::json& schema, const nlohmann::json& input,
                    std::string* reason, std::string* path) {
    try {
        nlohmann::json_schema::json_validator validator(nullptr, nullptr, check_content);
        validator.set_root_schema(schema);
        collecting_error_handler handler;
        validator.validate(input, handler);
        if (handler.failed() && reason) *reason = handler.reason();
        if (handler.failed() && path) *path = handler.path();
        return !handler.failed();
    } catch (const std::exception& error) {
        if (reason) *reason = std::string("the input could not be checked against its schema: ") + error.what();
        if (path) path->clear();
        return false;
    }
}

void audit_call(Node& node, const Capability& capability,
                const std::string& acting_key, bool allowed,
                const std::string& detail) {
    zetta::governance::AuditEventValue event;
    event.action = zetta::governance::AuditAction::DataAccess;
    event.actor = acting_key.empty() ? "local" : acting_key;
    event.target = "api:" + capability.name;
    event.allowed = allowed;
    event.detail = detail;
    node.record_audit_event(std::move(event));
}

bool audit_result_has_data(const Capability& capability,
                           const ApiResult& result) {
    if (!result.value.is_object() || capability.audit_result_field.empty()) {
        return false;
    }
    const auto found = result.value.find(capability.audit_result_field);
    if (found == result.value.end() || found->is_null()) {
        return false;
    }
    if (found->is_array() || found->is_object() || found->is_string()) {
        return !found->empty();
    }
    return true;
}

// The consent trail (#621): the challenge under the requester, the decision
// under the approver — audited on both sides.
void audit_consent(Node& node, zetta::governance::AuditAction action,
                   const std::string& actor, const std::string& capability_name,
                   bool allowed, const std::string& detail) {
    zetta::governance::AuditEventValue event;
    event.action = action;
    event.actor = actor.empty() ? "local" : actor;
    event.target = "api:" + capability_name;
    event.allowed = allowed;
    event.detail = detail;
    node.record_audit_event(std::move(event));
}

// The principal behind an acting key, for the different-principal approval
// rule. Empty for the unauthenticated local session or an unknown key.
std::string principal_of(Node& node, const std::string& acting_key) {
    if (acting_key.empty()) return {};
    if (auto key = node.find_access_key(acting_key)) return key->principal_id();
    return {};
}

}  // namespace

namespace {

// Run a handler, turning an exception into an engine_error result (#1007).
//
// Handlers are non-throwing by contract, but a handler reads replicated state a
// peer or a customer's dataset wrote — a column of an unexpected type, a value of
// the wrong shape — and nlohmann's typed accessors throw on that. An exception
// escaping Registry::call unwinds through the owner-loop bridge: over REST the
// caller gets no response at all, and on a headless node it terminates the
// process. Contain it here, the one place every surface passes through, and
// answer with the engine's reason like any other failure. (On macOS this relies
// on linking Homebrew's libunwind — see the note in CMakeLists.txt.)
ApiResult invoke_contained(const Capability& capability, const nlohmann::json& input,
                           ApiContext& context) {
    try {
        return capability.handler(input, context);
    } catch (const std::exception& error) {
        return ApiResult::failure(
            "engine_error", "'" + capability.name + "' failed: " + error.what());
    } catch (...) {
        return ApiResult::failure(
            "engine_error", "'" + capability.name + "' failed with an unknown error");
    }
}

}  // namespace

ApiResult Registry::call(const std::string& name, const nlohmann::json& input,
                         ApiContext& context) const {
    // Every call is a span (docs/architecture/observability.md): intent is
    // the reserved cross-capability field, stripped before schema validation;
    // status, duration, and result shape close it out. The trace ids ride
    // back on the ApiResult so the caller can read the full trace.
    zetta::observability::ScopedSpan span(context.recorder, context.trace,
                                          "api:" + name);
    span.set_attribute("capability", name);
    span.set_attribute("actor",
                       context.acting_key.empty() ? "local" : context.acting_key);
    // Everything this call records on the audit trail — including events the
    // Node records deep inside the handler (a deploy, a created project, a
    // queued job) — is attributed to the caller (#847).
    const zetta::governance::AuditActorScope audit_actor{context.acting_key};

    auto finish = [&](ApiResult result) {
        result.trace_id = context.trace.trace_id;
        result.span_id = span.span_id();
        result.duration_ms = span.elapsed_ms();
        if (result.ok) {
            nlohmann::json result_shape = nlohmann::json::object();
            if (result.value.is_object()) {
                for (const auto& [key, field] : result.value.items()) {
                    if (field.is_array()) result_shape[key] = field.size();
                }
                if (result.value.contains("rows"))
                    result_shape["rows"] = result.value["rows"].size();
            }
            span.set_attribute("result_shape", result_shape);
            span.finish(/*ok=*/true);
        } else {
            span.finish(/*ok=*/false,
                        result.error.code + ": " + result.error.message);
        }
        return result;
    };
    auto capability = find(name);
    if (!capability) {
        return finish(ApiResult::failure(
            "unknown_capability", "no capability named '" + name + "'"));
    }
    span.set_attribute("kind", std::string(to_string(capability->kind)));

    nlohmann::json effective_input = input;
    if (effective_input.is_object() && effective_input.contains("intent")) {
        span.set_attribute("intent", effective_input["intent"]);
        effective_input.erase("intent");
    }

    std::string reason;
    std::string path;
    if (!validate_input(capability->input_schema, effective_input, &reason,
                        &path)) {
        if (capability->invalid_input_hint) {
            if (const std::string hint =
                    capability->invalid_input_hint(effective_input);
                !hint.empty()) {
                reason += "; " + hint;
            }
        }
        return finish(ApiResult::failure("invalid_input", reason, path));
    }

    // Authorization (deny-by-default). Under the default-deny gate (#745) the
    // acting identity is a TOKEN-resolved user; otherwise the legacy key path
    // holds (a named AccessKey must carry the right; an empty acting key is the
    // audited "unauthenticated local" session — only reachable with the gate off).
    // Either way the decision is audited.
    //
    // The invoke right for the MCP/REST surface is `mcp:call` ({api,query}). Under
    // #747 that right is object-scopable PER TOOL, so for a user the invoke check
    // is `may_call_tool` (a flat mcp:call covers every tool — the built-ins have
    // it — while `mcp:call ON TOOL x` covers only x). A capability that needs a
    // MORE-specific right than the bare invoke (the orchestration actions) also
    // requires that right; capabilities gated on {api,query} need only the
    // per-tool invoke right (may_call_tool subsumes the coarse check).
    //
    // The gate reads the Node's keys and grants and appends to its audit trail,
    // both owner-thread state that the tick reconciles. An offloadable
    // capability reaches here on a pool worker, so the gate runs on the owner
    // thread: without that, a worker's audit append raced the tick's audit
    // reconciliation and crashed the node (#1063). run_on_owner runs inline
    // when already on the owner thread.
    std::optional<ApiResult> refused;
    context.node.run_on_owner([&] { refused = [&]() -> std::optional<ApiResult> {
        const zetta::governance::Operation kInvokeRight{
            zetta::governance::CapabilityDomain::Api, "query"};
        if (context.acting_is_user) {
            if (!context.node.may_call_tool(context.acting_key,
                                            /*acting_is_user=*/true, name)) {
                const std::string reason = "no mcp:call grant for tool '" + name + "'";
                audit_call(context.node, *capability, context.acting_key,
                           /*allowed=*/false, reason);
                return ApiResult::failure("unauthorized", reason);
            }
            if (!(capability->required_operation == kInvokeRight)) {
                auto decision = context.node.authorize_user(
                    context.acting_key, capability->required_operation);
                const bool scoped = !decision && capability->authorize_scoped &&
                    capability->authorize_scoped(effective_input, context);
                if (!decision && !scoped) {
                    audit_call(context.node, *capability, context.acting_key,
                               /*allowed=*/false, decision.reason);
                    return ApiResult::failure("unauthorized", decision.reason);
                }
            }
        } else if (!context.acting_key.empty()) {
            auto decision =
                context.node.authorize(context.acting_key, capability->required_operation);
            if (!decision) {
                audit_call(context.node, *capability, context.acting_key,
                           /*allowed=*/false, decision.reason);
                return ApiResult::failure("unauthorized", decision.reason);
            }
        }
        if (capability->audit_policy == AuditPolicy::PerCall) {
            audit_call(context.node, *capability, context.acting_key,
                       /*allowed=*/true,
                       std::string(to_string(capability->kind)) + " call");
        }
        return std::nullopt;
    }(); });
    if (refused) return finish(std::move(*refused));

    // Consent-gated capabilities never execute on a plain call (#621, the
    // cloud hard rule over the wire): when the input touches a cloud, issue a
    // one-time challenge naming exactly what would be read/run and return it
    // as a structured "consent_required" failure. Approval must come through
    // decide_consent, from a distinctly-privileged principal — this same key
    // repeating the call only ever mints another challenge.
    if (capability->requires_consent) {
        std::optional<zetta::governance::ConsentAction> action;
        if (capability->describe_consent) {
            action = capability->describe_consent(effective_input, context);
        } else {
            action = zetta::governance::ConsentAction{capability->description,
                                                      {}, {}};
        }
        // Consent enforcement is OFF by default (#995, NodeConfig::enforce_consent);
        // `--governance` (or ZETTA_GOVERNANCE=1) turns it on. Remote reads are
        // guarded per database by safe mode instead (docs/api/safe-mode.md).
        // Ungated is not unrecorded — the bypass is its own audit row
        // (`consent_bypassed`) carrying the action summary the approver would
        // have read, and a span attribute, so a node running without the gate
        // still shows every cloud-touching call that ran because of it.
        if (action && !context.node.config().enforce_consent) {
            span.set_attribute("consent_enforced", "false");
            audit_consent(context.node,
                          zetta::governance::AuditAction::ConsentBypassed,
                          context.acting_key, name, /*allowed=*/true,
                          "consent enforcement is disabled (--governance off); "
                          "ran without approval: " + action->summary);
            action.reset();
        }
        if (action) {
            auto challenge = context.node.consent_ledger().create(
                name, effective_input, std::move(*action), context.acting_key,
                principal_of(context.node, context.acting_key), context.acting_is_user);
            span.set_attribute("consent_challenge", challenge->id);
            audit_consent(context.node,
                          zetta::governance::AuditAction::ConsentRequested,
                          context.acting_key, name, /*allowed=*/false,
                          "challenge " + challenge->id + " issued: " +
                              challenge->action.summary);
            // Say where to go, not just what happened. A refusal that names a challenge id and
            // leaves the reader to find the surface that decides it is a dead end — the id is
            // useless without the page. Every surface named here can decide it (#999): the web
            // console's and the desktop app's Governance page both list pending challenges with
            // Approve / Deny, and approve_consent decides one over the wire.
            ApiResult refusal = ApiResult::failure(
                "consent_required",
                "'" + name + "' touches a cloud provider and did NOT execute; consent challenge " +
                    challenge->id +
                    " was issued and is waiting for a decision. Decide it under Governance "
                    "\u2192 Consent requests: in the web console (/?view=governance&tab=consent) "
                    "a DIFFERENT user than the requester holding cloud:manage approves or denies "
                    "it; in the headed (desktop) app the person at the machine can approve it "
                    "even on a one-person node. Over the wire, approve_consent from a key of a "
                    "different principal decides it. Approving from the session that made the "
                    "request is refused by design, and approvals are per-action and never "
                    "remembered. Read the pending challenges with list_consent_requests. "
                    "This node was started with --governance; without it consent gating "
                    "is off and this call would have executed.");
            refusal.error.details = zetta::governance::to_json(*challenge);
            return finish(std::move(refusal));
        }
        // The describer says this input touches no cloud (e.g. the `local`
        // target) — it executes like any other call.
    }

    // A handler that must run another capability for the caller (chat's read-only
    // agent tools, #904) calls back through this — same context, so the caller's
    // permissions and the audit trail apply to the inner call too.
    context.registry = this;
    ApiResult result = invoke_contained(*capability, effective_input, context);

    if (capability->audit_policy == AuditPolicy::SummarizeAuthorizedReads) {
        context.node.run_on_owner([&] {
            if (result.ok) {
                std::string scope = "*";
                if (!capability->audit_scope_field.empty() &&
                    effective_input.contains(capability->audit_scope_field) &&
                    effective_input.at(capability->audit_scope_field).is_string()) {
                    scope = effective_input.at(capability->audit_scope_field)
                                .get<std::string>();
                }
                std::uint64_t events_delivered = 0;
                if (result.value.is_object() &&
                    result.value.contains("events") &&
                    result.value.at("events").is_array()) {
                    events_delivered = result.value.at("events").size();
                }
                context.node.record_authorized_read(
                    context.acting_key, capability->name, std::move(scope),
                    events_delivered,
                    audit_result_has_data(*capability, result));
            } else {
                audit_call(context.node, *capability, context.acting_key,
                           result.error.code != "unauthorized",
                           result.error.code + ": " + result.error.message);
            }
        });
    }

    // A per-call capability is audited at authorization, before it runs, so a
    // call that then FAILED would read on the trail exactly like one that
    // succeeded. Record the failure of anything that is not a plain read (#847):
    // the trail must say an action was attempted and did not happen.
    if (capability->audit_policy == AuditPolicy::PerCall && !result.ok &&
        (capability->kind != CapabilityKind::Read || capability->mutates)) {
        context.node.run_on_owner([&] {
            audit_call(context.node, *capability, context.acting_key,
                       result.error.code != "unauthorized",
                       "failed: " + result.error.code + ": " + result.error.message);
        });
    }

    if (capability->audit_policy == AuditPolicy::HandlerRecordsSuccess &&
        !result.ok) {
        context.node.run_on_owner([&] {
            audit_call(context.node, *capability, context.acting_key,
                       result.error.code != "unauthorized",
                       result.error.code + ": " + result.error.message);
        });
    }

    if (result.ok && capability->mutates && activity_) {
        const ActivityEvent event{
            capability->name,
            context.acting_key.empty() ? "local" : context.acting_key};
        if (capability->blocks) {
            context.node.run_on_owner([&] {
                activity_(event);
            });
        } else {
            activity_(event);
        }
    }
    return finish(std::move(result));
}

ApiResult Registry::decide_consent(const std::string& challenge_id,
                                   bool approve, const std::string& reason,
                                   ApiContext& context,
                                   ConsentSurface surface) const {
    // The decision is a trace event of its own (#621: the challenge and the
    // approval are both spans), threaded back like every call.
    zetta::observability::ScopedSpan span(context.recorder, context.trace,
                                          "api:consent_decision");
    span.set_attribute("consent_challenge", challenge_id);
    span.set_attribute("decision", approve ? "approve" : "deny");
    span.set_attribute("surface", surface == ConsentSurface::HeadedApp
                                      ? "headed_app"
                                      : "approver_key");
    span.set_attribute("actor",
                       context.acting_key.empty() ? "local" : context.acting_key);

    auto finish = [&](ApiResult result) {
        result.trace_id = context.trace.trace_id;
        result.span_id = span.span_id();
        result.duration_ms = span.elapsed_ms();
        if (result.ok) {
            span.finish(/*ok=*/true);
        } else {
            span.finish(/*ok=*/false,
                        result.error.code + ": " + result.error.message);
        }
        return result;
    };

    auto& ledger = context.node.consent_ledger();
    auto challenge = ledger.find(challenge_id);
    if (!challenge) {
        return finish(ApiResult::failure(
            "not_found", "no consent challenge with id '" + challenge_id + "'"));
    }
    span.set_attribute("capability", challenge->capability);

    // The distinctly-privileged-approval rule: over the wire the decider must
    // be a NAMED key, and approval must come from a DIFFERENT principal than
    // the requester — the calling key can never satisfy its own challenge.
    // (The headed surface is the trusted human-in-front-of-the-machine path.)
    // Denial is fail-safe, so the requesting key may withdraw its own
    // challenge; approval never relaxes.
    const std::string decided_by = surface == ConsentSurface::HeadedApp
                                       ? "headed app"
                                       : context.acting_key;
    if (surface == ConsentSurface::ApproverKey) {
        if (context.acting_key.empty()) {
            return finish(ApiResult::failure(
                "unauthorized",
                "the unauthenticated local session cannot decide a consent "
                "challenge; use a named approver key or the headed app's "
                "approval surface"));
        }
        const bool self = context.acting_key == challenge->requested_by;
        if (approve && self) {
            return finish(ApiResult::failure(
                "unauthorized",
                "the calling key can never approve its own cloud action "
                "(challenge '" + challenge_id + "' was requested by this key)"));
        }
        if (approve && !challenge->requested_principal.empty() &&
            principal_of(context.node, context.acting_key) ==
                challenge->requested_principal) {
            return finish(ApiResult::failure(
                "unauthorized",
                "approval must come from a different principal than the "
                "requester; this key belongs to the same principal"));
        }
    }

    if (!approve) {
        auto denied = ledger.deny(challenge_id, decided_by,
                                  reason.empty() ? "denied" : reason);
        if (!denied) {
            return finish(ApiResult::failure("engine_error", denied.reason));
        }
        audit_consent(context.node,
                      zetta::governance::AuditAction::ConsentDecided,
                      decided_by, challenge->capability, /*allowed=*/false,
                      "challenge " + challenge_id + " denied: " +
                          (reason.empty() ? "denied" : reason));
        return finish(ApiResult::success(
            {{"challenge_id", challenge_id}, {"status", "denied"}}));
    }

    auto entry = find(challenge->capability);
    if (!entry) {
        return finish(ApiResult::failure(
            "unknown_capability",
            "challenge '" + challenge_id + "' names capability '" +
                challenge->capability + "', which is not registered here"));
    }

    // Consume the challenge — the one-time-use guarantee. From here the named
    // action runs at most once, whatever happens to this call.
    auto consumed = ledger.consume_for_execution(challenge_id, decided_by);
    if (!consumed) {
        return finish(ApiResult::failure("engine_error", consumed.reason));
    }
    audit_consent(context.node, zetta::governance::AuditAction::ConsentDecided,
                  decided_by, challenge->capability, /*allowed=*/true,
                  "challenge " + challenge_id + " approved; executing the "
                  "named action once");

    // Re-authorize the requester at execution time (its key may have been
    // dropped or its capability expired since the challenge was issued). A
    // token-resolved user is checked the way Registry::call checks it — the
    // per-tool invoke right, then the capability's own right — since a user id
    // is not an access key and the key path would refuse every user (#999).
    if (!challenge->requested_by.empty()) {
        zetta::governance::AccessDecision decision{true, {}};
        if (challenge->requested_is_user) {
            const zetta::governance::Operation kInvokeRight{
                zetta::governance::CapabilityDomain::Api, "query"};
            if (!context.node.may_call_tool(challenge->requested_by,
                                            /*acting_is_user=*/true, entry->name)) {
                decision = {false, "no mcp:call grant for tool '" + entry->name + "'"};
            } else if (!(entry->required_operation == kInvokeRight)) {
                decision = context.node.authorize_user(challenge->requested_by,
                                                       entry->required_operation);
            }
        } else {
            decision = context.node.authorize(challenge->requested_by,
                                              entry->required_operation);
        }
        if (!decision) {
            ledger.record_outcome(challenge_id, /*ok=*/false, {},
                                  "requester no longer authorized: " +
                                      decision.reason);
            audit_call(context.node, *entry, challenge->requested_by,
                       /*allowed=*/false, decision.reason);
            return finish(ApiResult::failure(
                "unauthorized",
                "the requesting key is no longer authorized: " + decision.reason));
        }
    }

    // Execute the FROZEN input under the requester's identity: the approver
    // authorizes, the requester acts. The execution is its own span; the
    // audit lands under the requester (both sides are now on the trail).
    ApiContext execution = context;
    execution.acting_key = challenge->requested_by;
    execution.acting_is_user = challenge->requested_is_user;
    execution.consent_challenge_id = challenge->id;
    execution.consent_outcome_deferred = false;
    zetta::observability::ScopedSpan action_span(
        context.recorder, context.trace, "api:" + entry->name);
    action_span.set_attribute("capability", entry->name);
    action_span.set_attribute("actor", challenge->requested_by.empty()
                                           ? "local"
                                           : challenge->requested_by);
    action_span.set_attribute("consent_challenge", challenge->id);
    action_span.set_attribute("approved_by", decided_by);
    audit_call(context.node, *entry, challenge->requested_by, /*allowed=*/true,
               "consent-approved execution of challenge " + challenge_id);

    const zetta::governance::AuditActorScope audit_actor{challenge->requested_by};
    ApiResult result = entry->handler(challenge->input, execution);
    action_span.finish(result.ok, result.ok ? std::string{}
                                            : result.error.code + ": " +
                                                  result.error.message);

    if (result.ok && entry->mutates && activity_) {
        activity_(ActivityEvent{entry->name, challenge->requested_by.empty()
                                                 ? "local"
                                                 : challenge->requested_by});
    }
    if (!execution.consent_outcome_deferred) {
        ledger.record_outcome(challenge_id, result.ok, result.value,
                              result.ok ? std::string{}
                                        : result.error.code + ": " +
                                              result.error.message);
    }

    if (!result.ok) {
        // The approval stood, but the named action failed — surface the
        // engine's reason verbatim (the challenge records the same outcome).
        ApiResult failure = ApiResult::failure(result.error.code,
                                               result.error.message,
                                               result.error.path);
        failure.error.details = {{"challenge_id", challenge_id},
                                 {"status", "failed"}};
        return finish(std::move(failure));
    }
    nlohmann::json output;
    output["challenge_id"] = challenge_id;
    output["capability"] = entry->name;
    output["status"] =
        execution.consent_outcome_deferred ? "executing" : "executed";
    output["result"] = result.value;
    return finish(ApiResult::success(std::move(output)));
}

}  // namespace zetta::api
