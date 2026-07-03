#include "zetta/observability/feedback.hpp"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json-schema.hpp>

#include "zetta/node.hpp"
#include "zetta/observability/crash.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/version.hpp"

namespace zetta::observability {

namespace {

// Caps keep a bundle bounded even on a busy node: enough context to
// reproduce, never an unbounded dump.
constexpr std::size_t kMaxAttachedTraces = 8;
constexpr std::size_t kMaxErrorSpans = 50;
constexpr std::size_t kMaxCrashReports = 10;

const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.FeedbackBundle",
      "type": "object",
      "properties": {
        "description":  { "type": "string", "minLength": 1 },
        "submitted_at": { "type": "string" },
        "actor":        { "type": "string" },
        "version":      { "type": "string" },
        "platform":     { "type": "string" },
        "node_id":      { "type": "string" },
        "traces": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "trace_id": { "type": "string" },
              "spans":    { "type": "array", "items": { "type": "object" } }
            },
            "required": ["trace_id", "spans"],
            "additionalProperties": false
          }
        },
        "recent_errors": { "type": "array", "items": { "type": "object" } },
        "crashes":       { "type": "array", "items": { "type": "object" } }
      },
      "required": ["description", "submitted_at", "version", "platform",
                   "traces", "recent_errors", "crashes"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

}  // namespace

const nlohmann::json& feedback_bundle_schema() { return schema_document(); }

std::optional<nlohmann::json> build_feedback_bundle(
    const Node& node, const FeedbackBundleOptions& options,
    std::string* error) {
    const SpanRecorder& recorder = node.span_recorder();

    // Which traces to attach: the caller's, or the most recent ones observed.
    std::vector<std::string> trace_ids = options.trace_ids;
    if (trace_ids.empty()) {
        std::unordered_set<std::string> seen;
        for (const Span& span : recorder.recent(200)) {
            if (seen.insert(span.trace_id).second) {
                trace_ids.push_back(span.trace_id);
            }
            if (trace_ids.size() >= kMaxAttachedTraces) break;
        }
    } else if (trace_ids.size() > kMaxAttachedTraces) {
        trace_ids.resize(kMaxAttachedTraces);
    }

    nlohmann::json traces = nlohmann::json::array();
    for (const std::string& trace_id : trace_ids) {
        nlohmann::json spans = nlohmann::json::array();
        for (const Span& span : recorder.trace(trace_id)) {
            spans.push_back(to_json(span));
        }
        traces.push_back(
            {{"trace_id", trace_id}, {"spans", std::move(spans)}});
    }

    nlohmann::json recent_errors = nlohmann::json::array();
    for (const Span& span : recorder.recent(500)) {
        if (span.ok) continue;
        recent_errors.push_back(to_json(span));
        if (recent_errors.size() >= kMaxErrorSpans) break;
    }

    nlohmann::json crashes = nlohmann::json::array();
    const std::string crashes_dir =
        (std::filesystem::path(node.config().data_dir) / "crashes").string();
    for (const LoadedCrashReport& loaded : load_crash_reports(crashes_dir)) {
        if (!loaded.report) continue;  // corrupt files stay on disk, not here
        crashes.push_back(to_json(*loaded.report));
        if (crashes.size() >= kMaxCrashReports) break;
    }

    nlohmann::json bundle{{"description", options.description},
                          {"submitted_at", zetta::util::now_iso8601()},
                          {"actor", options.actor},
                          {"version", zetta::kVersion},
                          {"platform", platform_string()},
                          {"node_id", node.node_id()},
                          {"traces", std::move(traces)},
                          {"recent_errors", std::move(recent_errors)},
                          {"crashes", std::move(crashes)}};

    collecting_error_handler handler;
    schema_validator().validate(bundle, handler);
    if (handler.failed) {
        if (error) *error = "feedback bundle failed its own schema: " + handler.reason;
        return std::nullopt;
    }
    return bundle;
}

}  // namespace zetta::observability
