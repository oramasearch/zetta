#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "zetta/ir/execution_graph.hpp"

// Compile a natural-language **intent** into a reviewable ExecutionGraph skeleton
// (an atomic slice of FEAT-2 / #311). This turns a stored intent — the text a
// `Project::capture_intent` recorded (FEAT-1) — into the first draft of an
// execution graph laid out over the connect→clean→model→index→serve→act taxonomy,
// using a **templated, heuristic** mapping rather than an opaque model.
//
// The output is the ExecutionGraph IR itself (ir/execution_graph.hpp): a
// well-formed graph of named, typed, editable steps. It is **not a black box** —
// once stored under a Project (`create_graph`) it is the same record the Graph
// Workspace panel renders and lets the user edit, add to, and delete from. The
// compiler only seeds a sensible starting point; review + edit is expected.
//
// Altitude: this slice is **templated mapping**, not language understanding. The
// heuristic recognizes the churn shape (the R1 use case, UC-1) from a handful of
// signal words and emits the full churn pipeline; anything else falls back to a
// generic-but-complete six-stage skeleton so the result is always well-formed and
// editable. Real NL parsing / LLM-driven synthesis plugs in later behind the same
// `compile_intent` front door without changing its contract.
namespace zetta {

// The template the heuristic mapped an intent onto — recorded so a caller (and a
// test) can see *why* a given skeleton was produced, keeping the mapping
// inspectable rather than magic.
enum class IntentShape {
    Churn,    // a churn / retention / attrition intent → the churn pipeline
    Generic,  // anything else → a generic connect→…→act skeleton
};

// The lowercase canonical name of an intent shape ("churn", "generic"). Used for
// labels, log lines, and test output.
std::string_view to_string(IntentShape shape);

// The result of compiling an intent: the skeleton graph plus the template that
// produced it. Returned by value; the graph is the editable artifact.
struct CompiledIntent {
    ir::ExecutionGraph graph;  // the templated skeleton (connect→…→act)
    IntentShape shape;         // which template the heuristic selected
};

// Compile a natural-language intent into a templated ExecutionGraph skeleton.
//
// Returns std::nullopt for an intent that is empty or only whitespace (it carries
// no meaning — nothing to compile), mirroring `Project::capture_intent`'s
// rejection; when `error` is non-null it is set to a human-readable reason. On
// success the returned graph is always well-formed (it is built through the IR's
// structure-guarding API, so it round-trips through ir::to_json/from_json and
// passes the baseline correctness gate) and carries one node per pipeline stage
// joined by data-flow edges. Deterministic: the same intent yields an identical
// graph (stable ids, names, and insertion order).
std::optional<CompiledIntent> compile_intent(std::string_view intent,
                                             std::string* error = nullptr);

}  // namespace zetta
