// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "zetta/ir/execution_graph.hpp"  // ir::ExecutionGraph + ir::StepKind

// Compile an imported n8n workflow JSON into the ExecutionGraph IR — the first
// concrete *compiler* of FEAT-2 (issue #308). The n8n import slice (#310,
// n8n_import.hpp) stores a workflow byte-for-byte as an artifact ProjectInput but
// never interprets it; this is the step that turns those bytes into a graph the
// gate, deploy, and run all build on:
//
//   - each n8n **node** becomes a typed IR step, keyed by its (unique) node name,
//     tagged with a StepKind from the connect→clean→model→index→serve→act
//     taxonomy (see n8n_step_kind);
//   - each n8n **connection** becomes a DataFlow edge between those steps.
//
// It is **non-throwing** and validates at the boundary, matching zetta's
// no-exceptions convention (`ir::from_json`): the workflow is first checked
// against the n8n workflow schema (validate_n8n_workflow), then the graph it
// describes is checked against the IR's structural rules, and any malformed or
// unsupported input is rejected cleanly with a human-readable reason instead of
// producing a half-built graph.
//
// R1 covers the node kinds in the churn workflows (the fixture
// tests/fixtures/churn_workflow.json); n8n_step_kind's table is the extension
// point as the use-case catalog grows. See docs/pipeline/n8n-compile.md.
namespace zetta::pipeline {

class ProjectInput;

// Map an n8n node `type` (e.g. "n8n-nodes-base.postgres") to its ExecutionGraph
// StepKind. Returns std::nullopt for a type R1 does not know how to compile yet —
// the lookup table is the single, documented source of truth and the place to
// extend coverage (compile_n8n_workflow fails with a clear reason on an unmapped
// type rather than guessing a stage).
std::optional<ir::StepKind> n8n_step_kind(std::string_view n8n_type);

// Compile an n8n workflow JSON string into an ExecutionGraph IR. On success
// returns the graph; on malformed input returns std::nullopt and, when `error` is
// non-null, sets it to a human-readable reason:
//   - "invalid JSON"
//   - "not an n8n workflow: <where>: <why>"          (schema mismatch)
//   - 'nodes[2]: unsupported n8n node type "…" (node "…")'
//   - 'nodes[1]: duplicate node name "…"'
//   - 'connections["A"]…: connection to unknown node "…"'
std::optional<ir::ExecutionGraph> compile_n8n_workflow(const std::string& content,
                                                       std::string* error = nullptr);

// Compile a stored n8n artifact ProjectInput by reading its payload bytes, then
// compiling as above. Fails (std::nullopt + *error) if the payload body is not
// present locally — e.g. on a peer that has the input's metadata but not yet the
// body (body transfer is a later slice).
std::optional<ir::ExecutionGraph> compile_n8n_workflow(const ProjectInput& input,
                                                       std::string* error = nullptr);

}  // namespace zetta::pipeline