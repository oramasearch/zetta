#pragma once

#include <string>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/end_state.hpp"
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

}  // namespace zetta::agent
