#pragma once

#include "zetta/agent/action.hpp"
#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/ir/execution_graph.hpp"

// The generic facts ↔ pipeline bridge (#873) — how ANY task's solution facts become a real,
// gradeable ir::ExecutionGraph, not just the hand-built churn/search graphs. This is what lets
// the whole 565-task corpus (and the plan's 24 use cases · 21 methods) carry a reference ACTION
// whose pipeline the gate scores, without a bespoke builder per use case.
//
//   build_solution_graph(facts) → a pipeline whose nodes carry the facts:
//     - a fact whose key is a step kind ("connect"/"clean"/"model"/"index"/"serve"/"act") and is
//       true becomes that step node (real pipeline structure);
//     - any other fact ("leakage_free", "model_kind", "ranked", "calibrated", …) is carried as an
//       attribute on the pipeline's solution node (a real pipeline's config IS its facts).
//   graph_solution_facts(graph) reads them back: step-kind presence + every node attribute.
//
// The round-trip is faithful by construction: graph_solution_facts(build_solution_graph(f)) ⊇ f,
// so a graph built from a task's reference facts passes that task's grader — the RLVR reward.
namespace zetta::agent {

// Build a pipeline that carries `facts` (see the header comment). Always has a Connect source.
ir::ExecutionGraph build_solution_graph(const EndState& facts);

// The facts of a pipeline: each node's step-kind presence + every node attribute. The inverse
// of build_solution_graph for the facts it carries (generic; works for any use case's graph).
EndState graph_solution_facts(const ir::ExecutionGraph& graph);

// The reference solution in the agent's ACTION shape, built generically from its `facts`: the
// build_solution_graph pipeline, one tool call per pipeline step, and a message. graph_solution_
// facts(action.execution_graph) satisfies `facts`, so the action passes the task's grader.
StudioAction reference_action_from_facts(const EndState& facts);

// Attach a generic reference action (from the case's reference facts) to an admission case, so
// the corpus carries an action the SFT render can target. Overwrites any existing action.
void attach_reference_action(AdmissionCase& admission_case);

}  // namespace zetta::agent
