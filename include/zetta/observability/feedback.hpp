#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/observability/trace.hpp"

// Feedback bundles (docs/architecture/observability.md, #640): the user (or
// agent) describes a bug or a wish, and the node snapshots the description
// together with the relevant traces, the recent error spans, the crash
// reports (diagnostic administrators only), and version/platform. A caller
// without the diagnostic grant gets only its OWN traces and errors (#1455). The bundle
// is stored as a large object the user EXPLICITLY
// exports; nothing ever leaves the cluster on its own (no self-egress).
namespace zetta {
class Node;
}

namespace zetta::observability {

// Which of the node's diagnostics a bundle may carry (#1408, #1455).
enum class FeedbackDiagnostics {
    none,  // description + version/platform only
    own,   // + the traces and error spans that are the actor's own, never a crash
    all,   // + any trace, every recent error and the crash reports (cluster:admin)
};

struct FeedbackBundleOptions {
    std::string description;             // the user's words, verbatim (required)
    std::vector<std::string> trace_ids;  // traces to attach; empty = the most
                                         // recent traces in the ring
    std::string actor;                   // acting key id, or "local"
    // Trusted internal state, never read from the wire. `all` only after the
    // caller is checked for cluster:admin (#1408); `own` for every other caller
    // (#1455). The default omits all traces, recent errors and crash reports.
    FeedbackDiagnostics diagnostics = FeedbackDiagnostics::none;
};

// Whether a trace is `actor`'s own: at least one of its spans names an actor,
// and every span that names one names `actor`. A trace a caller merely joined
// (an inbound traceparent can name anyone's trace id) carries the other
// actor's span, so it is not the caller's.
bool trace_is_owned_by(const std::vector<Span>& spans, const std::string& actor);

// Snapshot a feedback bundle from this node's observable state: description +
// attached traces (each trace id with its spans) + recent error spans +
// crash reports (only with FeedbackDiagnostics::all) + version/platform. The result
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
