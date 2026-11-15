// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string_view>

#include "zetta/input/language_frontend.hpp"
#include "zetta/ir/step_kind.hpp"

// A **simulated-Python frontend** (ENT-38 / ADR-33): parse a small, well-defined
// subset of Python and lower it to the ExecutionGraph IR. The supported subset is
//
//   - assignments               ds = load_dataset("…")
//   - function calls            train(ds, target="outcome")
//   - attribute / method access df.dropna()      a.b.c()
//   - method chaining           read_csv("x").dropna().fillna(0)
//   - literals                  strings, numbers, True/False/None, lists, dicts
//   - import / from-import      recorded as provenance, no step produced
//
// It is **not** a Python interpreter — it runs none of the source language and
// none of its libraries (the new-input principle: re-implement, don't run). It
// recognizes the *shape* of a pipeline expressed in Python and maps it to typed
// IR steps, the way `compile_n8n_workflow` maps an n8n workflow:
//
//   - every call becomes a typed `ir::Node` step. Its `StepKind` comes from the
//     called function/method name via `python_step_kind` (the documented verb
//     table); a name the table doesn't know defaults to `StepKind::Clean` (a
//     generic transform), so an arbitrary script still lowers to a well-formed
//     graph rather than failing — known verbs still get their precise stage.
//   - data flow becomes `DataFlow` edges. An assignment binds a variable to the
//     node a call produces; a later call that references that variable (as an
//     argument or as a method receiver) gets an edge from the producer node, so a
//     chain `a = f(); b = g(a)` is `f → g`.
//
// The output is the IR itself — named, typed, editable steps — and it round-trips
// through `ir::to_json`/`from_json` and passes the baseline correctness gate by
// construction, so the graph executes through `Node::run_graph` like any other
// frontend's. This adds **no new JSON format**: the input is Python source text
// and the only JSON it emits is the already-schema-validated IR JSON form (the
// `ir::from_json` trust boundary), with parsed call arguments recorded under a
// namespaced `python` attribute bag for provenance (the same shape n8n uses).
//
// Wiring this into a top-level input dispatch (so the GUI query box / new-project
// import auto-detects Python vs SQL) is a later slice. See
// docs/pipeline/python-frontend.md.
namespace zetta::input {

// Map a Python call's function/method name to its ExecutionGraph `StepKind`. The
// table is the documented extension point as the use-case catalog grows; it is
// keyed on the bare verb (`load_dataset`, `train`, `dropna`, …), case-sensitive.
// Returns std::nullopt for a verb the table does not know — `PythonFrontend`
// treats that as `StepKind::Clean` (a generic transform), but the lookup itself
// is honest about a miss so callers + tests can see the recognized vocabulary.
std::optional<ir::StepKind> python_step_kind(std::string_view verb);

// The simulated-Python LanguageFrontend. Stateless; construct one and call
// compile() (the same shape as `query::PostgresDialect`).
class PythonFrontend final : public LanguageFrontend {
public:
    const char* name() const override { return "python"; }
    FrontendResult compile(std::string_view source) const override;
};

}  // namespace zetta::input
