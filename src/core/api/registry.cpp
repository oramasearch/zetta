#include "zetta/api/registry.hpp"

#include <algorithm>
#include <exception>
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
// collected through the validator's error_handler seam — never thrown across
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
        }
    }
    bool failed() const { return failed_; }
    const std::string& reason() const { return reason_; }

private:
    bool failed_ = false;
    std::string reason_;
};

bool validate_input(const nlohmann::json& schema, const nlohmann::json& input,
                    std::string* reason) {
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);
    collecting_error_handler handler;
    validator.validate(input, handler);
    if (handler.failed() && reason) *reason = handler.reason();
    return !handler.failed();
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
    if (!validate_input(capability->input_schema, effective_input, &reason)) {
        return finish(ApiResult::failure("invalid_input", reason));
    }

    // Authorization: an empty acting key is the audited "unauthenticated
    // local" session (the governed-console precedent — the default loopback
    // binding); any named key must hold the entry's required capability
    // (deny-by-default). Either way the decision lands in the audit trail.
    if (!context.acting_key.empty()) {
        auto decision =
            context.node.authorize(context.acting_key, capability->required_operation);
        if (!decision) {
            audit_call(context.node, *capability, context.acting_key,
                       /*allowed=*/false, decision.reason);
            return finish(ApiResult::failure("unauthorized", decision.reason));
        }
    }
    audit_call(context.node, *capability, context.acting_key, /*allowed=*/true,
               std::string(to_string(capability->kind)) + " call");

    // Consent-gated capabilities never execute on a plain call: the challenge
    // protocol (#621) lands with the cloud tools; until then the refusal is
    // explicit rather than a silent gap.
    if (capability->requires_consent) {
        return finish(ApiResult::failure(
            "consent_required",
            "'" + name + "' touches a cloud provider and requires an approved "
            "consent challenge; the approval flow is not wired yet (#621)"));
    }

    ApiResult result = capability->handler(effective_input, context);

    if (result.ok && capability->mutates && activity_) {
        activity_(ActivityEvent{capability->name,
                                context.acting_key.empty() ? "local"
                                                           : context.acting_key});
    }
    return finish(std::move(result));
}

}  // namespace zetta::api
