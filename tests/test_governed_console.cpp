#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/capability.hpp"
#include "zetta/gate.hpp"
#include "zetta/node.hpp"
#include "zetta/principal.hpp"
#include "zetta/query/service.hpp"
#include "zetta/repl/console_model.hpp"
#include "zetta/repl/governed_console.hpp"
#include "zetta/scope.hpp"
#include "zetta/validation.hpp"

using zetta::AuditAction;
using zetta::CapabilityDomain;
using zetta::CapabilitySet;
using zetta::CheckResult;
using zetta::CheckStatus;
using zetta::CorrectnessGate;
using zetta::make_check;
using zetta::Node;
using zetta::NodeConfig;
using zetta::Operation;
using zetta::PrincipalKind;
using zetta::Scope;
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

// The last audit event the node recorded.
std::shared_ptr<zetta::AuditEvent> last_event(const Node& node) {
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
    ASSERT_EQ(node.audit_event_count(), 1u);

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
    ASSERT_EQ(node.audit_event_count(), 2u);
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

    // The action is recorded + allowed.
    ASSERT_EQ(node.audit_event_count(), 1u);
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

TEST(GovernedConsole, CapabilityGrantAllowsTheRun) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    zetta::query::QueryService service{node};
    GovernedConsole console{node, service};

    auto principal = node.create_principal("operator", PrincipalKind::Service);
    ASSERT_TRUE(principal);
    CapabilitySet caps;
    caps.grant(CapabilityDomain::Api, "query")
        .grant(CapabilityDomain::Orchestration, "start_run");
    auto key = principal->create_key(Scope{}, caps);
    ASSERT_TRUE(key);
    console.set_session_key(key->id());

    const Result sql = console.run(Frontend::Sql, kSqlSetup);
    EXPECT_TRUE(sql.ok()) << sql.error;
    const Result py = console.run(Frontend::SimPython, kPython);
    EXPECT_TRUE(py.ok()) << py.error;
    EXPECT_EQ(node.run_count(), 1u);

    // Both ran + are audited as allowed against the key.
    ASSERT_EQ(node.audit_event_count(), 2u);
    for (const auto& event : node.audit_events()) {
        EXPECT_TRUE(event->allowed());
        EXPECT_EQ(event->actor(), key->id());
    }

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
