// SPDX-License-Identifier: MIT

#include "zetta/repl/governed_console.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/governance/access_key.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/util/duration.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/pipeline/validation.hpp"

namespace zetta::repl {

namespace {

// Stringify one JSON cell for the result grid: a string verbatim (no quotes),
// null as blank, anything else as its compact JSON dump. (Mirrors the mapping the
// GUI's SQL console used before it moved here, so SQL rows render identically.)
std::string json_cell(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "";
    return value.dump();
}

// The first UUID segment of an id — enough to identify a run / step at a glance,
// matching how the GUI shows short ids elsewhere.
std::string short_id(const std::string& id) {
    const std::size_t dash = id.find('-');
    return dash == std::string::npos ? id : id.substr(0, dash);
}

// A one-line, length-capped summary of the entered source for the audit detail:
// the first non-blank line, trimmed, truncated with an ellipsis.
std::string summarize(const std::string& source) {
    std::size_t begin = source.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    std::size_t end = source.find_first_of("\r\n", begin);
    std::string line = source.substr(begin, end == std::string::npos
                                                ? std::string::npos
                                                : end - begin);
    constexpr std::size_t kMax = 60;
    if (line.size() > kMax) line = line.substr(0, kMax) + "…";
    return line;
}

// The first failing check's "<name>: <reason>" — the gate-block reason surfaced
// to the author (FEAT-3: a reason, never a stack trace).
std::string first_failure(const zetta::pipeline::ValidationReport& report) {
    for (const zetta::pipeline::CheckResult& check : report.checks) {
        if (check.status == zetta::pipeline::CheckStatus::Fail) {
            return check.name + ": " + check.reason;
        }
    }
    return "a correctness check failed";
}

// Map the query service's Outcome onto the frontend-agnostic Result the console
// renders: status messages + a column-aligned row grid on success, the first
// parse/execute error otherwise.
Result sql_result(query::QueryService::Outcome outcome) {
    Result result;
    result.elapsed = zetta::util::format_duration(outcome.elapsed);
    if (!outcome.ok()) {
        result.error = std::move(outcome.error);
        return result;
    }
    result.messages = std::move(outcome.messages);
    result.columns = std::move(outcome.columns);
    // `SELECT *` leaves the Outcome's columns empty (the rows carry the keys), so
    // derive the header from the union of row keys in first-seen order — otherwise
    // a `SELECT *` would render no grid.
    if (result.columns.empty()) {
        for (const nlohmann::json& row : outcome.rows) {
            if (!row.is_object()) continue;
            for (auto field = row.begin(); field != row.end(); ++field) {
                if (std::find(result.columns.begin(), result.columns.end(),
                              field.key()) == result.columns.end()) {
                    result.columns.push_back(field.key());
                }
            }
        }
    }
    result.rows.reserve(outcome.rows.size());
    for (const nlohmann::json& row : outcome.rows) {
        std::vector<std::string> cells;
        cells.reserve(result.columns.size());
        for (const std::string& column : result.columns) {
            if (row.is_object() && row.contains(column)) {
                cells.push_back(json_cell(row.at(column)));
            } else if (result.columns.size() == 1 && !row.is_object()) {
                cells.push_back(json_cell(row));  // a single scalar projection
            } else {
                cells.push_back("");
            }
        }
        result.rows.push_back(std::move(cells));
    }
    return result;
}

// Present an executed sim-Python graph as a grid of its lowered IR steps plus a
// summary of the produced Run — the tabular result an ad-hoc Python statement
// returns.
Result sim_python_result(const ir::ExecutionGraph& graph,
                         const zetta::pipeline::ExecutionResult& exec) {
    Result result;
    result.columns = {"step", "kind", "name"};
    for (const auto& step : graph.nodes()) {
        result.rows.push_back({short_id(step->id),
                               std::string(ir::to_string(step->kind)),
                               step->name});
    }
    result.messages.push_back("lowered " +
                              std::to_string(graph.nodes().size()) + " steps");
    result.messages.push_back("gate: passed");
    if (exec.run) {
        result.messages.push_back("run " + short_id(exec.run->id()) + ": " +
                                  std::string(to_string(exec.run->status())));
    }
    // A permitted run that then failed to execute surfaces its reason inline.
    if (!exec.ok) result.error = exec.error;
    return result;
}

}  // namespace

zetta::governance::Operation required_operation(Frontend frontend) {
    switch (frontend) {
        case Frontend::Sql:
            // Issue a query — an API-surface right (the documented "query" action).
            return zetta::governance::Operation{zetta::governance::CapabilityDomain::Api, "query"};
        case Frontend::SimPython:
            // Lower + run a graph — an orchestration right (start a run).
            return zetta::governance::Operation{zetta::governance::CapabilityDomain::Orchestration, "start_run"};
    }
    return zetta::governance::Operation{zetta::governance::CapabilityDomain::Api, "query"};
}

GovernedConsole::GovernedConsole(Node& node, query::QueryService& query,
                                 zetta::pipeline::CorrectnessGate gate)
    : node_(node), query_(query), gate_(std::move(gate)) {}

Result GovernedConsole::run(Frontend frontend, const std::string& source) {
    const std::string now = zetta::util::now_iso8601();
    const zetta::governance::Operation op = required_operation(frontend);
    const std::string frontend_name(to_string(frontend));
    const std::string summary = summarize(source);

    // ----- resolve the acting identity + the capability (verb/right) decision ---
    // An empty session key is an unauthenticated local session: it runs without
    // an enforcement check but is still audited. A set key is enforced — a key
    // that doesn't exist, or doesn't hold the required capability, is denied.
    std::string actor = "(local)";
    bool permitted = true;
    std::string deny_reason;
    // The principal steps and SQL run as: the user, else the key, else the trusted local session.
    const bool as_user = !session_user_.empty();
    const std::string principal = as_user ? session_user_ : session_key_;
    if (as_user) {
        actor = session_user_;
        const zetta::governance::AccessDecision decision = node_.authorize_user(session_user_, op);
        permitted = decision.allowed;
        if (!permitted) deny_reason = decision.reason;
    } else if (!session_key_.empty()) {
        actor = session_key_;
        const auto key = node_.find_access_key(session_key_);
        if (!key) {
            permitted = false;
            deny_reason = "unknown key";
        } else {
            const zetta::governance::AccessDecision decision = authorize(*key, op, now);
            permitted = decision.allowed;
            if (!permitted) deny_reason = decision.reason;
        }
    }

    const std::string target =
        frontend == Frontend::Sql
            ? (database_.empty() ? std::string("sql") : database_)
            : std::string("sim-python");

    // One immutable `data_access` audit event per statement (FEAT-9): the acting
    // key, what it touched, the governance decision, and a one-line summary.
    auto record = [&](bool allowed, std::string detail) {
        zetta::governance::AuditEventValue event;
        event.action = zetta::governance::AuditAction::DataAccess;
        event.actor = actor;
        event.target = target;
        event.allowed = allowed;
        event.detail = std::move(detail);
        event.time = now;
        node_.record_audit_event(std::move(event));
    };

    // ----- 1) capability gate: a denied statement runs no engine work ----------
    if (!permitted) {
        record(false, "console " + frontend_name + " denied · " + summary);
        return Result::failure(
            "blocked: " + (deny_reason.empty() ? "not permitted" : deny_reason) +
            (as_user ? " — user '" + session_user_ + "' lacks " : " — key '" + session_key_ + "' lacks ") +
            std::string(to_string(op.domain)) + ":" + op.action);
    }

    // ----- 2) execute, governed per frontend -----------------------------------
    switch (frontend) {
        case Frontend::Sql: {
            // A key session runs its statements as that key, so each one is
            // authorized on its own (sql:write / sql:ddl, #1229) — the gate
            // above only proves the key may query at all.
            Result result = sql_result(
                principal.empty()
                    ? query_.run(source, database_)
                    : query_.acting_as(principal, /*acting_is_user=*/as_user)
                          .run(source, database_));
            record(true, "console sql · " + summary +
                             (result.ok() ? "" : " · error: " + result.error));
            return result;
        }
        case Frontend::SimPython: {
            // Lower the source to the ExecutionGraph IR.
            const input::FrontendResult lowered = python_.compile(source);
            if (!lowered.ok()) {
                record(true, "console sim-python compile error · " + summary);
                return Result::failure("sim-python: " + lowered.error);
            }
            // Run the correctness gate (RULE-1) — a failing gate blocks the run.
            const zetta::pipeline::ValidationReport report = gate_.run(lowered.graph);
            if (report.block_deploy) {
                const std::string why = first_failure(report);
                record(false, "console sim-python blocked by gate · " + why);
                return Result::failure("blocked by gate: " + why);
            }
            // Gate passed → execute the lowered graph into a real, audited Run. The
            // graph runner is an injected dependency: by default the structural
            // DefaultStepExecutor (Node::run_graph), or a host-supplied runner that
            // routes a churn-shaped graph through the real churn engine on the node's
            // live JobCoordinator (#580). Governance (capability check above + the
            // audit record below) is identical either way.
            // A key session's steps run as that key (#843): each step must be
            // authorized for it before it executes. The local session is the
            // trusted principal and runs unchecked.
            const zetta::pipeline::ExecutionResult exec =
                graph_runner_ ? graph_runner_(lowered.graph)
                              : node_.run_graph(lowered.graph, {}, {},
                                                zetta::pipeline::RunPrincipal{
                                                    principal, /*acting_is_user=*/as_user});
            Result result = sim_python_result(lowered.graph, exec);
            record(true, "console sim-python · " + summary + " · run " +
                             (exec.run ? short_id(exec.run->id()) : "-") +
                             (exec.ok ? " ok" : " failed"));
            return result;
        }
    }
    return Result::failure("unknown frontend");
}

}  // namespace zetta::repl
