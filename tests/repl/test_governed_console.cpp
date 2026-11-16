#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/query/service.hpp"
#include "zetta/repl/console_model.hpp"
#include "zetta/repl/governed_console.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/pipeline/validation.hpp"

using zetta::governance::AuditAction;
using zetta::governance::CapabilityDomain;
using zetta::governance::CapabilitySet;
using zetta::pipeline::CheckResult;
using zetta::pipeline::CheckStatus;
using zetta::pipeline::CorrectnessGate;
using zetta::pipeline::make_check;
using zetta::Node;
using zetta::NodeConfig;
using zetta::governance::Operation;
using zetta::governance::PrincipalKind;
using zetta::governance::Scope;
using zetta::test::TempDir;
using zetta::repl::Frontend;
using zetta::repl::GovernedConsole;
using zetta::repl::required_operation;
using zetta::repl::Result;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A multi-statement SQL program that builds a table and reads it back.
const char* kSqlSetup =
    "CREATE DATABASE shop;\n"
    "CREATE TABLE shop.items (sku text, qty int);\n"
    "INSERT INTO shop.items (sku, qty) VALUES ('A1', 5), ('B2', 3);";
const char* kSqlSelect = "SELECT * FROM shop.items;";

// A tiny simulated-Python pipeline that lowers to a non-empty IR graph that
// passes the baseline gate by construction.
const char* kPython = "ds = load_dataset(\"orders\")\nmodel = train(ds)";

// How many audit events of one action the node holds — the console records a
// data_access row per run, and the executor a data_changed row per statement
// that changes data or schema (#1229).
std::size_t count_action(const Node& node, AuditAction action) {
    std::size_t count = 0;
    for (const auto& event : node.audit_events()) {
        if (event->action() == action) ++count;
    }
    return count;
}

// The last audit event the node recorded.
std::shared_ptr<zetta::governance::AuditEvent> last_event(const Node& node) {
    auto events = node.audit_events();
    return events.empty() ? nullptr : events.back();
}

}  // namespace

TEST(GovernedConsole, RequiredOperationByFrontend) {
    EXPECT_EQ(required_operation(Frontend::Sql),
              (Operation{CapabilityDomain::Api, "query"}));
    EXPECT_EQ(required_operation(Frontend::SimPython),
              (Operation{CapabilityDomain::Orchestration, "start_run"}));
}

// ----- SQL: tabular results + an audit record (the acceptance) ---------------

TEST(GovernedConsole, SqlRunsReturnsGridAndAudits) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};

    // Build the table (one run -> one audit event), then read it back.
    const Result setup = console.run(Frontend::Sql, kSqlSetup);
    EXPECT_TRUE(setup.ok()) << setup.error;
    ASSERT_EQ(count_action(node, AuditAction::DataAccess), 1u);
    // …and each of its three statements is an audited action (#1229), plus the
    // Node's own row for the database the setup created (#847).
    ASSERT_EQ(count_action(node, AuditAction::DataChanged), 4u);

    const Result select = console.run(Frontend::Sql, kSqlSelect);
    ASSERT_TRUE(select.ok()) << select.error;
    // `SELECT *` rows are JSON objects, so the derived header carries both columns
    // (the order is the engine's — JSON keys are alphabetical — so assert the set
    // and look cells up by column, not by a fixed position).
    ASSERT_EQ(select.columns.size(), 2u);
    const auto col = [&](const std::string& name) {
        return std::find(select.columns.begin(), select.columns.end(), name) -
               select.columns.begin();
    };
    ASSERT_NE(col("sku"), 2);
    ASSERT_NE(col("qty"), 2);
    ASSERT_EQ(select.rows.size(), 2u);
    EXPECT_EQ(select.rows[0][col("sku")], "A1");
    EXPECT_EQ(select.rows[0][col("qty")], "5");

    // The action is recorded in the audit trail (an unauthenticated local
    // session is allowed but still audited).
    ASSERT_EQ(count_action(node, AuditAction::DataAccess), 2u);
    ASSERT_EQ(count_action(node, AuditAction::DataChanged), 4u);  // 3 statements + the database (#847)
    auto event = last_event(node);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->action(), AuditAction::DataAccess);
    EXPECT_TRUE(event->allowed());
    EXPECT_EQ(event->actor(), "(local)");
    EXPECT_NE(event->detail().find("console sql"), std::string::npos);

    node.stop();
}

// ----- simulated-Python: lower to IR -> gate -> execute -> audit -------------

TEST(GovernedConsole, SimPythonLowersGatesExecutesAndAudits) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};

    EXPECT_EQ(node.run_count(), 0u);
    const Result result = console.run(Frontend::SimPython, kPython);

    ASSERT_TRUE(result.ok()) << result.error;
    // The result grid is the lowered IR steps.
    EXPECT_EQ(result.columns,
              (std::vector<std::string>{"step", "kind", "name"}));
    EXPECT_GE(result.rows.size(), 2u);  // load_dataset + train

    // It executed: a real Run was produced (so it shows in the Runs panel).
    EXPECT_EQ(node.run_count(), 1u);

    // The action is recorded + allowed: the Node's row for the run (#847), then
    // the console's row for the statement.
    ASSERT_EQ(node.audit_event_count(), 2u);
    EXPECT_EQ(node.audit_events().front()->action(), AuditAction::DataChanged);
    EXPECT_EQ(node.audit_events().front()->actor(), "local");
    auto event = last_event(node);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->action(), AuditAction::DataAccess);
    EXPECT_TRUE(event->allowed());
    EXPECT_NE(event->detail().find("console sim-python"), std::string::npos);

    node.stop();
}

TEST(GovernedConsole, SimPythonCompileErrorIsInlineNotABlock) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};

    // `def` is an explicitly-unsupported construct -> a compile error, not a run.
    const Result result = console.run(Frontend::SimPython, "def f(): pass");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("sim-python"), std::string::npos);
    EXPECT_EQ(node.run_count(), 0u);
    // A permitted-but-failed-to-compile statement is still audited (allowed).
    ASSERT_EQ(node.audit_event_count(), 1u);
    EXPECT_TRUE(last_event(node)->allowed());

    node.stop();
}

// ----- the correctness gate blocks the run (RULE-1) --------------------------

TEST(GovernedConsole, GateFailureBlocksTheRunAndAudits) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};

    // A gate whose only check always fails -> every sim-Python run is blocked.
    CorrectnessGate gate;
    gate.add_check(make_check("always_fail", [](const auto&) {
        return CheckResult{"always_fail", CheckStatus::Fail, "blocked for the test"};
    }));
    GovernedConsole console{node, service, gate};

    const Result result = console.run(Frontend::SimPython, kPython);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("blocked by gate"), std::string::npos);
    EXPECT_NE(result.error.find("always_fail"), std::string::npos);

    // Nothing executed; the block is audited as a denied data access.
    EXPECT_EQ(node.run_count(), 0u);
    ASSERT_EQ(node.audit_event_count(), 1u);
    auto event = last_event(node);
    ASSERT_TRUE(event);
    EXPECT_FALSE(event->allowed());
    EXPECT_NE(event->detail().find("blocked by gate"), std::string::npos);

    node.stop();
}

// ----- the injectable graph runner (#580 "run over the cluster" injection point) -----

// A gate-passed sim-Python graph runs through the injected graph runner (the injection point a
// host uses to route a churn-shaped graph through the real churn engine on the live
// JobCoordinator) — after the capability check + gate, before the audit record.
TEST(GovernedConsole, InjectedGraphRunnerExecutesGatePassedSimPython) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};

    int runner_calls = 0;
    console.set_graph_runner(
        [&](const zetta::ir::ExecutionGraph& graph) {
            ++runner_calls;
            return node.run_graph(graph);  // stands in for the real-engine route
        });

    const Result result = console.run(Frontend::SimPython, kPython);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(runner_calls, 1);       // the injected runner ran the gate-passed graph
    EXPECT_EQ(node.run_count(), 1u);  // a real Run was produced + audited
    ASSERT_EQ(node.audit_event_count(), 2u);  // the run's row (#847), the statement's
    EXPECT_TRUE(last_event(node)->allowed());

    node.stop();
}

// Governance precedes compute: a gate-blocked graph never reaches the runner.
TEST(GovernedConsole, InjectedGraphRunnerNotCalledWhenGateBlocks) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};

    CorrectnessGate gate;
    gate.add_check(make_check("always_fail", [](const auto&) {
        return CheckResult{"always_fail", CheckStatus::Fail, "blocked for the test"};
    }));
    GovernedConsole console{node, service, gate};

    int runner_calls = 0;
    console.set_graph_runner(
        [&](const zetta::ir::ExecutionGraph& graph) {
            ++runner_calls;
            return node.run_graph(graph);
        });

    const Result result = console.run(Frontend::SimPython, kPython);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(runner_calls, 0) << "the gate must block before the runner";
    EXPECT_EQ(node.run_count(), 0u);

    node.stop();
}

// ----- capability scoping ----------------------------------------------------

TEST(GovernedConsole, CapabilityDenyBlocksAndAudits) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};

    // A key that holds neither "query" (SQL) nor "start_run" (sim-Python).
    auto principal = node.create_principal("analyst", PrincipalKind::User);
    ASSERT_TRUE(principal);
    CapabilitySet caps;
    caps.grant(CapabilityDomain::Data, "read");
    auto key = principal->create_key(Scope{}, caps);
    ASSERT_TRUE(key);
    console.set_session_key(key->id());

    const Result sql = console.run(Frontend::Sql, kSqlSelect);
    EXPECT_FALSE(sql.ok());
    EXPECT_NE(sql.error.find("blocked"), std::string::npos);

    const Result py = console.run(Frontend::SimPython, kPython);
    EXPECT_FALSE(py.ok());

    // Neither ran; both denials are audited against the key.
    EXPECT_EQ(node.run_count(), 0u);
    ASSERT_EQ(node.audit_event_count(), 2u);
    for (const auto& event : node.audit_events()) {
        EXPECT_FALSE(event->allowed());
        EXPECT_EQ(event->actor(), key->id());
    }

    node.stop();
}

// LSR-261 (#1434): the web shell's console runs as the signed-in user. A user without the right is
// blocked, and the statement is audited under the user's id, never as the local session.
TEST(GovernedConsole, ASessionUserIsEnforcedAndAudited) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};
    auto user = node.create_user("web-shell-user");
    ASSERT_TRUE(user);
    console.set_session_user(user->id());

    const Result py = console.run(Frontend::SimPython, kPython);
    EXPECT_FALSE(py.ok());
    EXPECT_NE(py.error.find("user"), std::string::npos) << py.error;
    EXPECT_EQ(node.run_count(), 0u);
    ASSERT_EQ(node.audit_event_count(), 1u);
    for (const auto& event : node.audit_events()) {
        EXPECT_FALSE(event->allowed());
        EXPECT_EQ(event->actor(), user->id());
    }
    node.stop();
}

TEST(GovernedConsole, CapabilityGrantAllowsTheRun) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};

    auto principal = node.create_principal("operator", PrincipalKind::Service);
    ASSERT_TRUE(principal);
    CapabilitySet caps;
    // The setup creates a database and a table and inserts: the key needs the
    // schema and write rights, not only the query right (#1229). The pipeline's
    // steps run as the key too (#843): its connect reads, its model trains.
    caps.grant(CapabilityDomain::Api, "query")
        .grant(CapabilityDomain::Orchestration, "start_run")
        .grant(CapabilityDomain::Orchestration, "train_model")
        .grant(CapabilityDomain::Data, "read")
        .grant(CapabilityDomain::Data, "ddl")
        .grant(CapabilityDomain::Data, "write");
    auto key = principal->create_key(Scope{}, caps);
    ASSERT_TRUE(key);
    console.set_session_key(key->id());

    const Result sql = console.run(Frontend::Sql, kSqlSetup);
    EXPECT_TRUE(sql.ok()) << sql.error;
    const Result py = console.run(Frontend::SimPython, kPython);
    EXPECT_TRUE(py.ok()) << py.error;
    EXPECT_EQ(node.run_count(), 1u);

    // Both ran + are audited as allowed against the key: one data_access per
    // run, one per pipeline step the key was authorized for (#843: connect and
    // model), one data_changed per setup statement, and the Node's own rows for
    // the database created and the graph run (#847), all naming the key.
    ASSERT_EQ(count_action(node, AuditAction::DataAccess), 4u);
    ASSERT_EQ(count_action(node, AuditAction::DataChanged), 5u);
    for (const auto& event : node.audit_events()) {
        EXPECT_TRUE(event->allowed());
        EXPECT_EQ(event->actor(), key->id());
    }

    node.stop();
}

// A key that may query but not write: the console lets it in, and the engine
// refuses each data-changing statement as that key (#1229). It used to run as
// the trusted local session once past the query check.
TEST(GovernedConsole, QueryOnlyKeyCannotChangeData) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};
    // The table exists (built by the trusted local session).
    ASSERT_TRUE(console.run(Frontend::Sql, kSqlSetup).ok());

    auto principal = node.create_principal("viewer", PrincipalKind::Service);
    ASSERT_TRUE(principal);
    CapabilitySet caps;
    caps.grant(CapabilityDomain::Api, "query");
    auto key = principal->create_key(Scope{}, caps);
    ASSERT_TRUE(key);
    console.set_session_key(key->id());

    // A SELECT needs the key's own sql:read too (#762): with only api:query the
    // engine refuses it, where the key used to read the table whole.
    const Result unread = console.run(Frontend::Sql, kSqlSelect);
    EXPECT_FALSE(unread.ok());
    EXPECT_NE(unread.error.find("permission denied"), std::string::npos) << unread.error;
    caps.grant(CapabilityDomain::Data, "read");
    key = principal->create_key(Scope{}, caps);
    ASSERT_TRUE(key);
    console.set_session_key(key->id());

    for (const char* statement :
         {"INSERT INTO shop.items (sku, qty) VALUES ('C3', 1);",
          "UPDATE shop.items SET qty = 0;", "DELETE FROM shop.items;",
          "TRUNCATE shop.items;", "CREATE TABLE shop.more (a int);",
          "DROP TABLE shop.items;", "DROP DATABASE shop;"}) {
        const Result refused = console.run(Frontend::Sql, statement);
        EXPECT_FALSE(refused.ok()) << statement;
        EXPECT_NE(refused.error.find("permission denied"), std::string::npos)
            << statement << ": " << refused.error;
    }
    // Reading is still allowed, and nothing changed.
    const Result select = console.run(Frontend::Sql, kSqlSelect);
    ASSERT_TRUE(select.ok()) << select.error;
    EXPECT_EQ(select.rows.size(), 2u);

    std::size_t refused_changes = 0;
    for (const auto& event : node.audit_events()) {
        if (event->action() == AuditAction::DataChanged && !event->allowed()) {
            EXPECT_EQ(event->actor(), key->id());
            ++refused_changes;
        }
    }
    EXPECT_EQ(refused_changes, 7u);
    node.stop();
}

TEST(GovernedConsole, UnknownSessionKeyIsDenied) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};
    console.set_session_key("no-such-key");

    const Result result = console.run(Frontend::Sql, kSqlSelect);
    EXPECT_FALSE(result.ok());  // deny-by-default for an unknown credential
    ASSERT_EQ(node.audit_event_count(), 1u);
    EXPECT_FALSE(last_event(node)->allowed());

    node.stop();
}
