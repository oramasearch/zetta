#include "zetta/intent_compiler.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/ir/step_kind.hpp"

namespace zetta {
namespace {

// A lowercase copy of `text`, so the keyword heuristic is case-insensitive. ASCII
// is enough here — the signal words are all ASCII.
std::string to_lower(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return lowered;
}

// Does the intent read like a churn / retention problem? The heuristic is a plain
// substring scan for the signal words that name this use case (UC-1). It is
// intentionally simple and transparent — the point of this slice is a *templated*
// mapping, not language understanding; a richer parser plugs in later behind the
// same compile_intent front door without changing its contract.
bool looks_like_churn(const std::string& lowered_intent) {
    static constexpr std::array<std::string_view, 11> kSignals = {
        "churn",   "retention", "retain",      "attrition", "cancel",
        "renew",   "renewal",   "subscriber",  "lapse",     "downgrade",
        "win-back"};
    return std::any_of(kSignals.begin(), kSignals.end(),
                       [&](std::string_view signal) {
                           return lowered_intent.find(signal) !=
                                  std::string::npos;
                       });
}

// The full churn pipeline skeleton (connect→clean→model→index→serve→act), laid
// out as a senior data+AI engineer would seed it: two connect sources feeding a
// feature build, a churn-risk model, an index over its scores, a serving step,
// and a retention action. Every node is named + typed + editable; the attributes
// are starting-point hints (the `note` explains each step for review), all of
// which the user is expected to refine in the Graph Workspace.
ir::ExecutionGraph build_churn_skeleton() {
    using nlohmann::json;
    ir::ExecutionGraph graph;

    graph.add_node(ir::StepKind::Connect, "Load customer accounts",
                   json{{"source", "warehouse"},
                        {"table", "customers"},
                        {"note", "active accounts to score for churn risk"}},
                   "load_customers");
    graph.add_node(ir::StepKind::Connect, "Load usage & billing activity",
                   json{{"source", "warehouse"},
                        {"table", "events"},
                        {"note", "engagement + payment signals per account"}},
                   "load_activity");
    graph.add_node(
        ir::StepKind::Clean, "Build churn features",
        json{{"note", "point-in-time features; keep out post-cutoff leakage"}},
        "build_features");
    graph.add_node(ir::StepKind::Model, "Train churn-risk model",
                   json{{"algo", "gradient_boosting"},
                        {"target", "churned"},
                        {"note", "predict probability each account churns"}},
                   "train_churn_model");
    graph.add_node(ir::StepKind::Index, "Index churn scores",
                   json{{"kind", "feature"},
                        {"note", "make scores fast to look up at serve time"}},
                   "index_scores");
    graph.add_node(ir::StepKind::Serve, "Serve churn scores & reasons",
                   json{{"note", "expose per-account risk + top drivers"}},
                   "serve_scores");
    graph.add_node(ir::StepKind::Act, "Trigger retention outreach",
                   json{{"note", "notify retention on high-risk accounts"}},
                   "notify_retention");

    graph.add_edge("load_customers", "build_features");
    graph.add_edge("load_activity", "build_features");
    graph.add_edge("build_features", "train_churn_model");
    graph.add_edge("train_churn_model", "index_scores");
    graph.add_edge("index_scores", "serve_scores");
    graph.add_edge("serve_scores", "notify_retention");
    return graph;
}

// A generic-but-complete skeleton for an intent the heuristic doesn't recognize:
// one node per pipeline stage, chained linearly. It is still well-formed and
// editable, so an unrecognized intent yields a usable starting point rather than
// nothing (the user reshapes it in the workspace).
ir::ExecutionGraph build_generic_skeleton() {
    using nlohmann::json;
    ir::ExecutionGraph graph;

    graph.add_node(ir::StepKind::Connect, "Connect data source",
                   json{{"note", "pull in the data this project needs"}},
                   "connect_source");
    graph.add_node(ir::StepKind::Clean, "Clean & prepare data",
                   json{{"note", "normalize, dedupe, validate, reshape"}},
                   "clean_data");
    graph.add_node(ir::StepKind::Model, "Build model",
                   json{{"note", "train / evaluate / apply a model"}},
                   "build_model");
    graph.add_node(ir::StepKind::Index, "Build index",
                   json{{"note", "build a search / vector / feature index"}},
                   "build_index");
    graph.add_node(ir::StepKind::Serve, "Serve results",
                   json{{"note", "expose results behind a query / API"}},
                   "serve_results");
    graph.add_node(ir::StepKind::Act, "Take action",
                   json{{"note", "notify / write back / trigger downstream"}},
                   "take_action");

    graph.add_edge("connect_source", "clean_data");
    graph.add_edge("clean_data", "build_model");
    graph.add_edge("build_model", "build_index");
    graph.add_edge("build_index", "serve_results");
    graph.add_edge("serve_results", "take_action");
    return graph;
}

}  // namespace

std::string_view to_string(IntentShape shape) {
    switch (shape) {
        case IntentShape::Churn:   return "churn";
        case IntentShape::Generic: return "generic";
    }
    return "unknown";
}

std::optional<CompiledIntent> compile_intent(std::string_view intent,
                                             std::string* error) {
    // Reject empty / whitespace-only intent: there is nothing to compile, so we
    // return no graph (mirroring Project::capture_intent's rejection of empty
    // intent at ingestion). The caller decides what to do with the empty result.
    if (intent.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
        if (error) *error = "intent is empty";
        return std::nullopt;
    }

    const std::string lowered = to_lower(intent);
    if (looks_like_churn(lowered)) {
        return CompiledIntent{build_churn_skeleton(), IntentShape::Churn};
    }
    return CompiledIntent{build_generic_skeleton(), IntentShape::Generic};
}

}  // namespace zetta
