#pragma once

#include <functional>
#include <string>

#include "zetta/governance/capability.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/repl/console_model.hpp"

// The **governed console** (SCR-8 / #525) — the engine-agnostic governance
// pipeline behind the headed Console/REPL screen. It is the `repl::RunHandler`
// the [REPL console component](console_model.hpp) emits a run event to, but with
// the full FEAT-9 governance chain wired in: every entered statement is
// **capability-scoped**, the simulated-Python frontend **lowers to the
// ExecutionGraph IR and passes the correctness gate** before it runs, and **every
// statement is recorded in the audit trail**. It realizes **UC-3** — an ad-hoc
// question, governed.
//
// Like the `ConsoleModel` it feeds, it carries **no UI dependency** (no SDL /
// ImGui) and lives in zetta-core, so the same governance runs headless (a future
// CLI REPL, `tests/test_governed_console.cpp`) and under the GUI screen, which is
// a thin renderer that only sets the acting key + database and forwards each run.
//
// Two frontends, governed two ways (see docs/governance/governed-console.md):
//
//   - **SQL** runs through the dialect-agnostic [query service](../query/service.hpp)
//     — its own validated executor — so it is governed by the **capability check +
//     audit** (SQL does not lower to the IR; the query layer is its boundary).
//   - **Simulated-Python** lowers to the IR through `input::PythonFrontend`, is run
//     through the **correctness gate** (RULE-1 — a failing gate blocks the run),
//     then executes via the **graph runner** — by default `Node::run_graph` (the
//     structural DefaultStepExecutor), but a host may inject one that routes a
//     churn-shaped graph through the **real** churn engine on the node's live
//     `JobCoordinator` (#580; `run_churn_solution_on_node`), so an interactive run
//     trains a real Model over the live cluster. Either way it produces a real
//     (audited) Run, governed identically.
//
// It adds **no new JSON boundary**: SQL enters through `QueryService` (already
// validated at the query boundary), sim-Python through `ir::from_json` (the IR
// trust boundary) + the gate, and each audit event through the AuditEvent JSON
// Schema at `Node::record_audit_event`. A governed run is recorded as the existing
// `data_access` audit action (the FEAT-9 authorize seam): the verb/right axis
// (ENT-22) is enforced and the decision + a one-line summary land in the trail.
namespace zetta {

class Node;
namespace query {
class QueryService;
}

namespace repl {

// The capability (verb/right) a console statement needs, by frontend — the
// operation the acting key is checked against (ENT-22). SQL is an `api`/`query`
// right; simulated-Python runs a graph, an `orchestration`/`start_run` right.
// Exposed so the GUI selector + tests can name the rights a key must hold.
zetta::governance::Operation required_operation(Frontend frontend);

// The governance pipeline for one REPL/console session. Holds the engine seams a
// statement needs (the Node for run/audit/key lookup, the query service for SQL)
// plus the correctness gate; the acting key + current database are session state
// the host updates between statements. `run` matches `repl::RunHandler`, so it
// slots straight into `ConsoleModel::submit`.
class GovernedConsole {
public:
    // `gate` defaults to the baseline `default_gate()`; an injected gate lets a
    // test exercise the gate-block path deterministically.
    GovernedConsole(Node& node, query::QueryService& query,
                    zetta::pipeline::CorrectnessGate gate = zetta::pipeline::default_gate());

    // The acting key id ("" = an unauthenticated local session). When set, every
    // statement is capability-checked against this key before it runs and a denied
    // statement is blocked. When empty, the statement runs without an enforcement
    // check but is still audited (actor `(local)`), so the basic "see results"
    // path is frictionless while a selected key demonstrates the deny path.
    void set_session_key(std::string key_id) { session_key_ = std::move(key_id); }
    const std::string& session_key() const { return session_key_; }
    // A signed-in user instead of a key (LSR-261, #1434): every statement is authorized for that
    // user, runs as that user and is audited under the user's id, as a key session's are under
    // the key. Takes precedence over a session key.
    void set_session_user(std::string user_id) { session_user_ = std::move(user_id); }
    const std::string& session_user() const { return session_user_; }

    // The database unqualified SQL names resolve against (mirrors
    // `QueryService::run`'s current-database argument). Also the audit target for
    // a SQL statement.
    void set_database(std::string database) { database_ = std::move(database); }
    const std::string& database() const { return database_; }

    // How a gate-passed simulated-Python graph is executed into a Run. The seam the
    // sim-Python path runs the lowered graph through (after the capability check +
    // gate, before the audit record), so the governance chain is unchanged whether the
    // run is the structural placeholder or real compute. Defaults to `Node::run_graph`
    // (DefaultStepExecutor). A host injects one to route a churn-shaped graph through
    // the real churn engine on the node's live JobCoordinator (#580) — see
    // `run_churn_solution_on_node` / `is_churn_graph` in churn_solution.hpp.
    using GraphRunner = std::function<pipeline::ExecutionResult(const ir::ExecutionGraph&)>;
    void set_graph_runner(GraphRunner runner) {
        graph_runner_ = std::move(runner);
    }

    // Run one statement end-to-end, governed: capability check → (sim-python:
    // lower to IR + gate) → execute → record one `data_access` audit event. The
    // returned Result drives the console's transcript + result grid; a blocked
    // statement returns a failure Result and runs no engine work.
    Result run(Frontend frontend, const std::string& source);

private:
    Node& node_;
    query::QueryService& query_;
    input::PythonFrontend python_;
    zetta::pipeline::CorrectnessGate gate_;
    GraphRunner graph_runner_;  // how a gate-passed sim-Python graph runs (default: run_graph)
    std::string session_key_;  // "" = unauthenticated local session
    std::string session_user_;  // a signed-in user (#1434); "" = none
    std::string database_;     // current database for unqualified SQL names
};

}  // namespace repl
}  // namespace zetta
