// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <utility>

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
// A sentence describing the pipeline `facts` produce — the steps in order, then the guarantees
// worth stating back (validated on later data, calibrated, explained, approval-gated).
//
// Shared so a corpus family and the agent's own reply describe a pipeline the same way. Replaces a
// constant confirmation that once covered 1,802 cases: a constant is the cheapest thing a model can
// learn, and it learned it. Digit-free by construction — an action reply is not grounded in a
// measured result, so any figure in it would be one the engine never computed.
std::string describe_pipeline(const EndState& facts);

StudioAction reference_action_from_facts(const EndState& facts);

// Attach a generic reference action (from the case's reference facts) to an admission case, so
// the corpus carries an action the SFT render can target. Overwrites any existing action.
void attach_reference_action(AdmissionCase& admission_case);

// One SFT training example for the reshaped agent (#873): {prompt, target}. The prompt is the
// Studio system prompt + the task's ask (agent_prompt_format); the target is the reference
// ACTION as JSON — the case's own reference_action when set, else built from its facts. This is
// the single place the SFT render + any tokenized training pipeline get their (prompt, target),
// so training targets the action, not the facts.
std::pair<std::string, std::string> render_action_example(const AdmissionCase& admission_case);

// Build a Pipeline by executing tool calls in order — the harness's job, and the counterpart to
// the training target's shape.
//
// The model decides WHICH steps to call and with what configuration; this mints each node's id and
// wires the edge from the step before it. Keeping id generation here is the point: node ids are
// random UUIDs, so a model asked to write them can only learn what one looks like, which is how a
// checkpoint came to emit `11111111-…` for every node and collide two steps on one id. Shared by
// `agent::respond` and the corpus tests so both build a pipeline exactly the same way.
ir::ExecutionGraph build_pipeline_from_tool_calls(const std::vector<ToolCall>& calls);

// The tool calls that BUILD `graph` — one call per node, in pipeline order: the tool is the node's
// step kind, the arguments are its attributes plus its `name`.
//
// This is the exact inverse of build_pipeline_from_tool_calls, and that matters more than it
// looks. A pipeline has two encodings in a training target — the graph and the calls that build it
// — and a corpus that writes them independently WILL let them disagree. It did: 91% of the v1
// corpus's action cases carried a real graph and an empty `tool_calls`, so the corpus taught
// "an action calls nothing", and the trained checkpoint dutifully called nothing. Deriving one
// from the other is what makes that disagreement unrepresentable.
std::vector<ToolCall> tool_calls_from_graph(const ir::ExecutionGraph& graph);

}  // namespace zetta::agent
