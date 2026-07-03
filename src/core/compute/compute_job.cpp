#include "zetta/compute/compute_job.hpp"

#include "zetta/cluster/replication.hpp"
#include "zetta/util/timestamp.hpp"

#include "../catalog.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::compute {
namespace fs = std::filesystem;

namespace {

// The ComputeJob JSON Schema (draft-07). One declarative source of truth for the
// value shape carried by job.json (with an optional `id`) and the `job:<uuid>`
// CRDT value (without it). `kind` / `status` are closed enums and the shape is
// sealed (additionalProperties: false), so a malformed record fails validation at
// the boundary instead of corrupting the tree. This embedded schema is the source
// of truth; schemas/compute_job.schema.json is generated from it
// (tests/test_schemas.cpp) and a drift check keeps it in sync.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ComputeJob",
      "type": "object",
      "properties": {
        "id":      { "type": "string" },
        "kind":    { "type": "string",
                     "enum": ["featurize", "train", "explain", "score", "ddf"] },
        "status":  { "type": "string",
                     "enum": ["queued", "running", "succeeded", "failed"] },
        "project":        { "type": "string" },
        "graph":          { "type": "string" },
        "run":            { "type": "string" },
        "scaling_policy": { "type": "string" },
        "created":        { "type": "string" },
        "updated":        { "type": "string" },
        "detail":         { "type": "string" },
        "trace_id":       { "type": "string" }
      },
      "required": ["kind", "status"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
// validate() is then cheap and runs at every boundary.
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure. Using the error-handler overload of
// validate() (rather than the throwing one) keeps validation failures from
// crossing the validator-library boundary as exceptions — the library's
// recommended path, and robust regardless of how each side is compiled.
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

}  // namespace

std::string_view to_string(JobStatus status) {
    switch (status) {
        case JobStatus::Queued:    return "queued";
        case JobStatus::Running:   return "running";
        case JobStatus::Succeeded: return "succeeded";
        case JobStatus::Failed:    return "failed";
    }
    return "queued";
}

std::optional<JobStatus> job_status_from_string(std::string_view name) {
    if (name == "queued")    return JobStatus::Queued;
    if (name == "running")   return JobStatus::Running;
    if (name == "succeeded") return JobStatus::Succeeded;
    if (name == "failed")    return JobStatus::Failed;
    return std::nullopt;
}

std::string_view to_string(JobKind kind) {
    switch (kind) {
        case JobKind::Featurize: return "featurize";
        case JobKind::Train:     return "train";
        case JobKind::Explain:   return "explain";
        case JobKind::Score:     return "score";
        case JobKind::Ddf:       return "ddf";
    }
    return "featurize";
}

std::optional<JobKind> job_kind_from_string(std::string_view name) {
    if (name == "featurize") return JobKind::Featurize;
    if (name == "train")     return JobKind::Train;
    if (name == "explain")   return JobKind::Explain;
    if (name == "score")     return JobKind::Score;
    if (name == "ddf")       return JobKind::Ddf;
    return std::nullopt;
}

bool is_terminal(JobStatus status) {
    return status == JobStatus::Succeeded || status == JobStatus::Failed;
}

nlohmann::json to_json(const ComputeJobValue& value) {
    return nlohmann::json{
        {"kind", to_string(value.kind)},
        {"status", to_string(value.status)},
        {"project", value.project},
        {"graph", value.graph},
        {"run", value.run},
        {"scaling_policy", value.scaling_policy},
        {"created", value.created},
        {"updated", value.updated},
        {"detail", value.detail},
        {"trace_id", value.trace_id}};
}

const nlohmann::json& compute_job_value_schema() { return schema_document(); }

bool validate_compute_job_value(const nlohmann::json& value,
                                std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<ComputeJobValue> parse_compute_job_value(
    const nlohmann::json& value, std::string* error) {
    if (!validate_compute_job_value(value, error)) return std::nullopt;

    // The schema has already guaranteed types + the kind/status enums, so the
    // extraction below is total: missing optional fields fall back to defaults.
    ComputeJobValue parsed;
    parsed.kind = job_kind_from_string(value.value("kind", std::string{}))
                      .value_or(JobKind::Featurize);
    parsed.status =
        job_status_from_string(value.value("status", std::string{"queued"}))
            .value_or(JobStatus::Queued);
    parsed.project = value.value("project", std::string{});
    parsed.graph = value.value("graph", std::string{});
    parsed.run = value.value("run", std::string{});
    parsed.scaling_policy = value.value("scaling_policy", std::string{});
    parsed.created = value.value("created", std::string{});
    parsed.updated = value.value("updated", std::string{});
    parsed.detail = value.value("detail", std::string{});
    parsed.trace_id = value.value("trace_id", std::string{});
    return parsed;
}

// ----- ComputeJob ------------------------------------------------------------

ComputeJob::ComputeJob(std::string id, ComputeJobValue value, fs::path dir,
                       zetta::cluster::ReplicationManager* replication, zetta::governance::AuditSink* audit)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication),
      audit_(audit) {}

void ComputeJob::set_status(JobStatus status, std::string detail) {
    const bool was_terminal = is_terminal(value_.status);
    value_.status = status;
    if (!detail.empty()) value_.detail = std::move(detail);
    capture_and_persist();
    // Audit the finish (FEAT-9): a job reaching a terminal state is a governed
    // action. Emit once — only on the first crossing into terminal, and never
    // from apply_value (the reconcile path), where a peer's finish arrives
    // already-audited as its own replicated `audit:` event.
    if (audit_ && !was_terminal && is_terminal(value_.status)) {
        audit_->record_audit_event(
            {zetta::governance::AuditAction::JobFinished, {}, id_,
             value_.status == JobStatus::Succeeded,
             "compute job " + std::string(to_string(value_.kind)) + " " +
                 std::string(to_string(value_.status)) +
                 (value_.detail.empty() ? "" : " (" + value_.detail + ")"),
             {}});
    }
}

void ComputeJob::set_trace_id(std::string trace_id) {
    value_.trace_id = std::move(trace_id);
    capture_and_persist();
}

void ComputeJob::apply_value(ComputeJobValue value) {
    value_ = std::move(value);
    persist_meta();  // projection only — no capture / no audit (no feedback loop)
}

void ComputeJob::capture_and_persist() {
    value_.updated = zetta::util::now_iso8601();
    // CRDT first (the source of truth), then the on-disk projection.
    if (replication_) replication_->set(catalog::job_key(id_), metadata());
    persist_meta();
}

nlohmann::json ComputeJob::metadata() const { return to_json(value_); }

void ComputeJob::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "job.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::compute