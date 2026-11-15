// SPDX-License-Identifier: MIT

// Step authority (#843): a graph step runs as the run's principal, deny-by-default,
// checked when the step is reached, with every decision on the audit trail. These
// tests assert that a step the principal may not run is refused and never
// executes, through GraphExecutor, Node::run_graph, authorize_graph_steps, the
// governed console and the API's `run` capability.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/step_authority.hpp"
#include "zetta/query/service.hpp"
#include "zetta/repl/governed_console.hpp"

namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::governance::AuditAction;
using zetta::governance::CapabilityDomain;
using zetta::governance::CapabilitySet;
using zetta::governance::Operation;
using zetta::governance::PrincipalKind;
using zetta::governance::Resource;
using zetta::governance::Scope;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::pipeline::DefaultStepExecutor;
using zetta::pipeline::GraphExecutor;
using zetta::pipeline::RunPrincipal;
using zetta::pipeline::RunStatus;
using zetta::pipeline::StepExecutor;
using zetta::pipeline::StepOutcome;
using zetta::test::TempDir;

// Records which steps actually executed, so a test can prove a denied step never
// ran (not only that the run failed).
class RecordingStepExecutor final : public StepExecutor {
public:
    StepOutcome run_step(const zetta::ir::Node& step) const override {
        executed.push_back(step.name);
        return DefaultStepExecutor{}.run_step(step);
    }
    mutable std::vector<std::string> executed;
};

// connect(sales.customers) -> model.
ExecutionGraph connect_then_model(const std::string& table = "customers") {
    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "load",
                                  {{"source", "sales"}, {"table", table}});
    auto model = graph.add_node(StepKind::Model, "train");
    graph.add_edge(connect->id, model->id);
    return graph;
}

// One step of every kind, chained.
ExecutionGraph every_kind() {
    ExecutionGraph graph;
    std::shared_ptr<zetta::ir::Node> previous;
    for (const StepKind kind : zetta::ir::all_step_kinds()) {
        auto step = graph.add_node(kind, std::string(zetta::ir::to_string(kind)));
        if (previous) graph.add_edge(previous->id, step->id);
        previous = step;
    }
    return graph;
}

std::vector<std::shared_ptr<zetta::governance::AuditEvent>> step_events(const Node& node) {
    std::vector<std::shared_ptr<zetta::governance::AuditEvent>> out;
    for (const auto& event : node.audit_events()) {
        if (event->action() == AuditAction::DataAccess &&
            event->value().target.rfind("step:", 0) == 0) {
            out.push_back(event);
        }
    }
    return out;
}

struct StepAuthorityNode : ::testing::Test {
    TempDir dir;
    Node node{NodeConfig{dir.path().string(), /*preflight=*/false}};

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
    }
    void TearDown() override { node.stop(); }

    std::string user_with_role(const std::string& user_name, const std::string& role_name) {
        auto user = node.create_user(user_name);
        auto role = node.find_role(role_name);
        EXPECT_TRUE(user && role);
        node.assign_role(user->id(), role->id());
        return user->id();
    }
};

}  // namespace

TEST(StepAuthority, EveryStepKindNamesAnExistingRight) {
    EXPECT_EQ(zetta::pipeline::step_operation(StepKind::Connect),
              (Operation{CapabilityDomain::Data, "read"}));
    EXPECT_EQ(zetta::pipeline::step_operation(StepKind::Clean),
              (Operation{CapabilityDomain::Orchestration, "create_dataset_from_query"}));
    EXPECT_EQ(zetta::pipeline::step_operation(StepKind::Model),
              (Operation{CapabilityDomain::Orchestration, "train_model"}));
    EXPECT_EQ(zetta::pipeline::step_operation(StepKind::Index),
              (Operation{CapabilityDomain::Orchestration, "create_index"}));
    EXPECT_EQ(zetta::pipeline::step_operation(StepKind::Serve),
              (Operation{CapabilityDomain::Data, "predict"}));
    EXPECT_EQ(zetta::pipeline::step_operation(StepKind::Act),
              (Operation{CapabilityDomain::Orchestration, "deliver_action_task"}));
}

TEST_F(StepAuthorityNode, LocalPrincipalRunsEveryStepUnaudited) {
    auto steps = std::make_shared<RecordingStepExecutor>();
    const auto result = GraphExecutor{steps}.execute(node, every_kind());
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(steps->executed.size(), 6u);
    EXPECT_TRUE(step_events(node).empty());
}

TEST_F(StepAuthorityNode, AdminRunsEveryStepAndEachGrantIsAudited) {
    const std::string admin = user_with_role("ada", "admin");
    auto steps = std::make_shared<RecordingStepExecutor>();
    GraphExecutor executor{steps};
    executor.run_as({admin, /*acting_is_user=*/true});
    const auto result = executor.execute(node, every_kind());
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(steps->executed.size(), 6u);
    const auto events = step_events(node);
    ASSERT_EQ(events.size(), 6u);
    for (const auto& event : events) {
        EXPECT_TRUE(event->allowed());
        EXPECT_EQ(event->actor(), admin);
        EXPECT_NE(event->value().detail.find("run " + result.run->id()), std::string::npos);
    }
}

// The reader may read but not train: connect runs, the model step is refused
// before it executes, the run ends Failed on that step, and both decisions are on
// the audit trail.
TEST_F(StepAuthorityNode, UngrantedStepIsRefusedAndNeverExecutes) {
    const std::string reader = user_with_role("rhea", "reader");
    auto steps = std::make_shared<RecordingStepExecutor>();
    GraphExecutor executor{steps};
    executor.run_as({reader, /*acting_is_user=*/true});
    const ExecutionGraph graph = connect_then_model();
    const auto result = executor.execute(node, graph);

    EXPECT_FALSE(result.ok);
    ASSERT_TRUE(result.run);
    EXPECT_EQ(result.run->status(), RunStatus::Failed);
    EXPECT_EQ(result.failed_step, graph.nodes_of_kind(StepKind::Model).front()->id);
    EXPECT_NE(result.error.find("orchestration:train_model"), std::string::npos)
        << result.error;
    EXPECT_EQ(steps->executed, std::vector<std::string>{"load"});

    const auto events = step_events(node);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_TRUE(events[0]->allowed());
    EXPECT_EQ(events[0]->value().target, "step:connect:load");
    EXPECT_FALSE(events[1]->allowed());
    EXPECT_EQ(events[1]->actor(), reader);
    EXPECT_EQ(events[1]->value().target, "step:model:train");
    EXPECT_NE(events[1]->value().detail.find("denied"), std::string::npos);
}

TEST_F(StepAuthorityNode, UnknownPrincipalIsDeniedAtTheFirstStep) {
    auto steps = std::make_shared<RecordingStepExecutor>();
    GraphExecutor executor{steps};
    executor.run_as({"no-such-user", /*acting_is_user=*/true});
    const auto result = executor.execute(node, connect_then_model());
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(steps->executed.empty());
    const auto events = step_events(node);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_FALSE(events[0]->allowed());

    // The same for an AccessKey id that does not exist.
    auto key_steps = std::make_shared<RecordingStepExecutor>();
    GraphExecutor key_executor{key_steps};
    key_executor.run_as({"no-such-key", /*acting_is_user=*/false});
    EXPECT_FALSE(key_executor.execute(node, connect_then_model()).ok);
    EXPECT_TRUE(key_steps->executed.empty());
}

// A user whose read is scoped to one table may connect to that table and no
// other: the connect step is checked against the table it names.
TEST_F(StepAuthorityNode, ConnectStepIsCheckedAgainstTheTableItNames) {
    node.create_role("clerk");
    ASSERT_TRUE(node.grant_scoped_permissions("clerk", {"sql:read"},
                                              Resource::table("sales", "customers")));
    const std::string clerk = user_with_role("cleo", "clerk");
    const RunPrincipal principal{clerk, /*acting_is_user=*/true};

    const ExecutionGraph granted = connect_then_model("customers");
    const ExecutionGraph refused = connect_then_model("payroll");
    const auto& granted_connect = *granted.nodes_of_kind(StepKind::Connect).front();
    const auto& refused_connect = *refused.nodes_of_kind(StepKind::Connect).front();

    EXPECT_TRUE(zetta::pipeline::decide_step(node, principal, granted_connect));
    const auto denied = zetta::pipeline::decide_step(node, principal, refused_connect);
    EXPECT_FALSE(denied);
    EXPECT_NE(denied.reason.find("sales.payroll"), std::string::npos) << denied.reason;
}

TEST_F(StepAuthorityNode, AccessKeyStepsAreCheckedAgainstItsCapabilities) {
    auto service = node.create_principal("svc", PrincipalKind::Service);
    ASSERT_TRUE(service);
    CapabilitySet caps;
    caps.grant(CapabilityDomain::Data, "read");
    auto key = service->create_key(Scope{}, caps);
    ASSERT_TRUE(key);
    const RunPrincipal principal{key->id(), /*acting_is_user=*/false};

    const ExecutionGraph graph = connect_then_model();
    EXPECT_TRUE(zetta::pipeline::decide_step(
        node, principal, *graph.nodes_of_kind(StepKind::Connect).front()));
    EXPECT_FALSE(zetta::pipeline::decide_step(
        node, principal, *graph.nodes_of_kind(StepKind::Model).front()));
}

// The preflight used where a run does not walk its steps (a methodology's own
// callback, a composed solution's stages): it stops at the first denial.
TEST_F(StepAuthorityNode, AuthorizeGraphStepsStopsAtTheFirstDenial) {
    const std::string reader = user_with_role("rhea", "reader");
    const ExecutionGraph graph = every_kind();
    zetta::pipeline::GraphAuthorization authorized =
        zetta::pipeline::authorize_graph_steps(node, {reader, true}, graph);
    EXPECT_FALSE(authorized);
    // The reader holds data:read but no pipelines:manage: the clean step is the first refused.
    EXPECT_EQ(authorized.step_id, graph.nodes_of_kind(StepKind::Clean).front()->id);
    EXPECT_EQ(step_events(node).size(), 2u);  // connect granted, clean denied, no more

    const std::string admin = user_with_role("ada", "admin");
    EXPECT_TRUE(zetta::pipeline::authorize_graph_steps(node, {admin, true}, graph));
    EXPECT_TRUE(zetta::pipeline::authorize_graph_steps(node, {}, graph));  // local
}

TEST_F(StepAuthorityNode, NodeRunGraphRunsAsThePrincipal) {
    const std::string reader = user_with_role("rhea", "reader");
    const auto refused =
        node.run_graph(connect_then_model(), {}, {}, RunPrincipal{reader, true});
    EXPECT_FALSE(refused.ok);
    const std::string admin = user_with_role("ada", "admin");
    EXPECT_TRUE(node.run_graph(connect_then_model(), {}, {}, RunPrincipal{admin, true}).ok);
    EXPECT_TRUE(node.run_graph(connect_then_model()).ok);  // the local session
}

// A console key that may start a run but not train: the lowered pipeline's model
// step is refused. Before #843 the run executed every step as the node.
TEST_F(StepAuthorityNode, ConsoleKeyRunsItsStepsAsThatKey) {
    zetta::query::QueryService service{node};
    zetta::repl::GovernedConsole console{node, service};
    auto principal = node.create_principal("operator", PrincipalKind::Service);
    ASSERT_TRUE(principal);
    CapabilitySet caps;
    caps.grant(CapabilityDomain::Orchestration, "start_run")
        .grant(CapabilityDomain::Data, "read");
    auto key = principal->create_key(Scope{}, caps);
    ASSERT_TRUE(key);
    console.set_session_key(key->id());

    console.run(zetta::repl::Frontend::SimPython,
                "ds = load_dataset(\"orders\")\nmodel = train(ds)");
    ASSERT_EQ(node.run_count(), 1u);
    EXPECT_EQ(node.runs().front()->status(), RunStatus::Failed);
    bool denied_model = false;
    for (const auto& event : step_events(node)) {
        if (!event->allowed() && event->value().target.rfind("step:model:", 0) == 0 &&
            event->actor() == key->id()) {
            denied_model = true;
        }
    }
    EXPECT_TRUE(denied_model);
}

// The API's `run`: a user allowed to call `run` (pipelines:manage) but not to read
// data cannot make a connect step read for it.
TEST_F(StepAuthorityNode, ApiRunExecutesStepsAsTheCaller) {
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    const auto project = node.create_project("p843");
    ASSERT_TRUE(project);
    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "load",
                                  {{"source", "sales"}, {"table", "customers"}});
    auto clean = graph.add_node(StepKind::Clean, "tidy");
    graph.add_edge(connect->id, clean->id);
    ASSERT_TRUE(project->create_graph("g843", std::move(graph)));

    node.create_role("runner");
    ASSERT_TRUE(node.grant_permissions("runner", {"mcp:call", "pipelines:manage"}));
    const std::string runner = user_with_role("rex", "runner");

    zetta::api::ApiContext context{node, nullptr, runner,
                                   zetta::observability::make_root_trace(),
                                   &node.span_recorder()};
    context.acting_is_user = true;
    const auto refused =
        registry.call("run", {{"project", "p843"}, {"graph", "g843"}}, context);
    ASSERT_TRUE(refused.ok) << refused.error.message;  // the attempt is a recorded Run
    EXPECT_EQ(refused.value.at("status"), "failed");
    EXPECT_NE(refused.value.at("error").get<std::string>().find("data:read"),
              std::string::npos);
    const auto events = step_events(node);
    ASSERT_FALSE(events.empty());
    EXPECT_FALSE(events.back()->allowed());
    EXPECT_EQ(events.back()->actor(), runner);

    // Granting the read right lets the same caller run the same pipeline.
    ASSERT_TRUE(node.grant_permissions("runner", {"sql:read"}));
    const auto granted =
        registry.call("run", {{"project", "p843"}, {"graph", "g843"}}, context);
    ASSERT_TRUE(granted.ok) << granted.error.message;
    EXPECT_EQ(granted.value.at("status"), "succeeded");
}
