#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Feedback bundles (docs/architecture/observability.md, #640): the user (or
// agent) describes a bug or a wish, and the node snapshots the description
// together with the relevant traces, the recent error spans, the crash
// reports (diagnostic administrators only), and version/platform. The bundle
// is stored as a large object the user EXPLICITLY
// exports; nothing ever leaves the cluster on its own (no self-egress).
namespace zetta {
class Node;
}

namespace zetta::observability {

struct FeedbackBundleOptions {
    std::string description;             // the user's words, verbatim (required)
    std::vector<std::string> trace_ids;  // traces to attach; empty = the most
                                         // recent traces in the ring
    std::string actor;                   // acting key id, or "local"
    // Trusted callers opt in only after checking cluster:admin (#1408).
    // The default omits all traces, recent errors and crash reports.
    bool include_diagnostics = false;
};

// Snapshot a feedback bundle from this node's observable state: description +
// attached traces (each trace id with its spans) + recent error spans +
// crash reports (only with include_diagnostics) + version/platform. The result
// is validated against feedback_bundle_schema() before it is returned — a
// bundle that does not match its own schema is a bug, surfaced immediately
// (nullopt + reason).
std::optional<nlohmann::json> build_feedback_bundle(
    const Node& node, const FeedbackBundleOptions& options,
    std::string* error = nullptr);

// Schema-as-code for the bundle (draft-07; generated mirror
// schemas/feedback_bundle.schema.json).
const nlohmann::json& feedback_bundle_schema();

}  // namespace zetta::observability
