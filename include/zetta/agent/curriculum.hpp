#pragma once

#include <string>
#include <vector>

#include "zetta/agent/action.hpp"
#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/agent/tool_call.hpp"
#include "zetta/ir/execution_graph.hpp"

// The base-agent training curriculum: authored, gated seed tasks (#866). Each is a
// trainable, *discriminating* game in the blender-rl sense — it ships a reference
// solution that passes its grader and negative controls that fail (see the
// rlvr-training-data skill). The frontier-model generators grow the corpus around
// these seeds, and every generated task passes this same `admit_corpus` gate before it
// is trusted. This header exposes the native seed cases the corpus is grown from.
namespace zetta::agent {

// Sample #1 — Charles's churn game (Motive UC-1): the improvement over his current
// LLM-pattern-matching n8n. The required end-state is a *correct* churn solution —
// connect the source, engineer leakage-free features, train a gradient-boosted model
// with SHAP explanations, and serve calibrated risk scores with a score-vs-outcome
// calibration loop. The reference satisfies all of that; the negative controls (an
// LLM-only pipeline like his, a leaky one, a no-calibration one) each fail.
AdmissionCase churn_solution_case();

// Build a churn execution graph parametrically — the reference solution expressed as a
// series of `Connect → Clean → Model → Serve → Act` mutations over `ir::ExecutionGraph`.
// The parameters flip it between the correct solution and the degenerate variants used
// as negative controls (an LLM model, a leaky clean, a missing calibration loop).
ir::ExecutionGraph build_churn_graph(std::string model_kind, std::string explain,
                                     bool leakage_free, bool calibrated,
                                     bool calibration_loop);

// Extract the churn grader's facts (connect / leakage_free / model_kind / explain /
// calibrated / calibration_loop) from a built execution graph, so a task's reference is
// the facts of an *actually built* graph, not hand-written strings.
EndState churn_graph_facts(const ir::ExecutionGraph& graph);

// A Sandbox with the churn graph-building tools registered — connect/clean/model/serve/
// act — each mutating a shared `ir::ExecutionGraph` and refreshing the graded facts via
// `churn_graph_facts`. This is the rollout substrate a policy acts in: the agent emits
// tool calls, the sandbox builds the graph, the grader scores the resulting facts.
Sandbox churn_sandbox();

// The ordered tool calls that build the correct churn solution — the reference solution
// as the agent's own actions (the series of execution-graph mutations). Applied to a
// `churn_sandbox()`, they yield a graph whose facts pass the churn grader.
std::vector<ToolCall> churn_reference_trajectory();

// The churn reference in the Studio auto-AI agent's ACTION shape (#873): a user-facing
// `message`, the `tool_calls` that build the pipeline (churn_reference_trajectory), and the
// `execution_graph` (build_churn_graph). Its graph passes the churn grader — the RLVR reward
// (score `churn_graph_facts(action.execution_graph)` through churn_solution_case().task.grader()).
// The training target for the churn use case once the corpus emits actions instead of facts.
StudioAction churn_reference_action();

// --- Search (the second dogfood Component, #873) — the same pattern, a different shape ---
// Search pipelines DON'T train a model or calibrate — connect → index → serve. This is where
// "not every pipeline is six stages" is concrete: the search gate scores only search checks.

// The required facts of a correct search solution: a real index built, real retrieval, ranked
// results grounded in the indexed data (index_kind/embeddings vary by variant, so aren't
// required universally).
EndState search_required();

// Build a search pipeline: connect(documents) → index(kind) → serve(ranked, grounded). A real
// index kind (bm25f/semantic/hybrid) builds an Index node; a non-index kind (e.g. "llm",
// "substring") builds a Model node instead — a guess with no retrieval — so degenerate graphs
// fail the gate and the corpus can discriminate.
ir::ExecutionGraph build_search_graph(std::string index_kind, bool ranked, bool grounded);

// The facts of a built search graph (index/retrieval/ranked/grounded_in_data + index_kind/
// embeddings), scored against search_required().
EndState search_graph_facts(const ir::ExecutionGraph& graph);

// The admission case for search: a correct full-text reference + degenerate controls (an LLM
// guess with no index; unranked retrieval; retrieval not grounded in the data).
AdmissionCase search_solution_case();

// The search reference in the agent's ACTION shape (#873) — {message, tool_calls, a
// build_search_graph pipeline}. Its graph passes the search grader.
StudioAction search_reference_action();

}  // namespace zetta::agent
