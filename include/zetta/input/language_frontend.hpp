#pragma once

#include <string>
#include <string_view>

#include "zetta/ir/execution_graph.hpp"

// A **language frontend** turns a source snippet in some input language into the
// ExecutionGraph IR (ir/execution_graph.hpp) — the IR the gate, deploy, and run
// all build on (ENT-38 / ADR-33). It is the IR-targeting analogue of the query
// layer's `zetta::query::Dialect`, which turns SQL text into the generic query
// AST: add support for another input language by implementing this interface, and
// everything downstream (the gate, the GraphExecutor, the GUI) is reused as-is.
//
// zetta accepts input "from Postgres SQL to Python code." SQL-family input goes
// through a `query::Dialect` (it has its own executor); non-SQL input that
// describes a data/AI pipeline — Python, a config, an API shape — goes through a
// LanguageFrontend, lowering to the same IR a stored graph uses. The first
// implementation is `zetta::input::PythonFrontend` (simulated Python; see
// python_frontend.hpp + docs/python-frontend.md).
namespace zetta::input {

// The result of compiling a source snippet to the IR. Mirrors
// `query::ParseResult`: `error` is empty on success, and `graph` is meaningful
// only then. Returned by value; the graph is the editable artifact.
struct FrontendResult {
    ir::ExecutionGraph graph;  // the lowered graph (valid only when ok())
    std::string error;         // human-readable reason; empty on success

    bool ok() const { return error.empty(); }
};

// A language frontend: lower a source snippet to the ExecutionGraph IR.
//
// `compile` is **non-throwing**, matching zetta's convention for zetta-core
// (ir::from_json / compile_n8n_workflow are non-throwing too — the library is
// compiled with -fexperimental-library and must not unwind across its boundary).
// A malformed snippet is rejected cleanly with a reason in
// `FrontendResult::error` rather than producing a half-built graph.
class LanguageFrontend {
public:
    virtual ~LanguageFrontend() = default;

    // Short, stable name of the input language this frontend accepts, e.g.
    // "python" (used for logs, routing, and test output).
    virtual const char* name() const = 0;

    // Parse + lower `source` to the IR. On success FrontendResult::ok() is true
    // and `graph` holds the result; on failure `error` explains why.
    virtual FrontendResult compile(std::string_view source) const = 0;
};

}  // namespace zetta::input
