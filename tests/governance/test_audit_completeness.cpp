// SPDX-License-Identifier: MIT

// Audit completeness and attribution (#847): every change made through the
// Node's own API is on the trail, and every row names who did it — the caller
// the entry point declared (AuditActorScope), carried across to the owner
// thread, never left blank and never a stand-in such as the project's owner.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/query/service.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::governance::AuditAction;
using zetta::governance::AuditActorScope;
using zetta::governance::AuditEvent;
using zetta::governance::current_audit_actor;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::pipeline::DeploymentTarget;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    cfg.preflight = false;
    return cfg;
}

ExecutionGraph sample_graph() {
    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "load_orders");
    auto clean = graph.add_node(StepKind::Clean, "dedupe");
    auto model = graph.add_node(StepKind::Model, "churn_model");
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);
    return graph;
}

// The rows recorded with `action` on `target`, oldest first.
std::vector<std::shared_ptr<AuditEvent>> rows(const Node& node, AuditAction action,
                                              const std::string& target) {
    std::vector<std::shared_ptr<AuditEvent>> found;
    for (const auto& event : node.audit_events()) {
        if (event->action() == action && event->target() == target) found.push_back(event);
    }
    return found;
}

// Exactly one row for `target`, naming `actor`; returns its detail.
std::string expect_one(const Node& node, AuditAction action, const std::string& target,
                       const std::string& actor) {
    const auto found = rows(node, action, target);
    EXPECT_EQ(found.size(), 1u) << "rows for " << target;
    if (found.empty()) return {};
    EXPECT_EQ(found.front()->actor(), actor) << target;
    EXPECT_TRUE(found.front()->allowed()) << target;
    return found.front()->detail();
}

}  // namespace

TEST(AuditActorScope, NestsAndRestoresAndSpellsTheLocalSession) {
    EXPECT_FALSE(current_audit_actor());
    {
        const AuditActorScope outer{std::string("ada")};
        EXPECT_EQ(current_audit_actor(), "ada");
        {
            const AuditActorScope inner{std::string{}};
            EXPECT_EQ(current_audit_actor(), "local");
            {
                const AuditActorScope none{std::optional<std::string>{}};
                EXPECT_FALSE(current_audit_actor());
            }
            EXPECT_EQ(current_audit_actor(), "local");
        }
        EXPECT_EQ(current_audit_actor(), "ada");
        // Thread-local: another thread sees no actor.
        std::optional<std::string> seen = std::string("unset");
        std::thread([&] { seen = current_audit_actor(); }).join();
        EXPECT_FALSE(seen);
    }
    EXPECT_FALSE(current_audit_actor());
}

// The headed app calls the Node directly for these (src/gui/app_explorer.cpp,
// app_models.cpp, app_graph_workspace.cpp). Before #847 none of them left a row.
TEST(AuditCompleteness, CatalogChangesThroughTheNodeAreRecordedAndAttributed) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    const AuditActorScope desk{std::string("local")};

    ASSERT_TRUE(node.create_project("churn", "owner-key"));
    ASSERT_TRUE(node.rename_project("churn", "retention"));
    ASSERT_TRUE(node.create_database("warehouse"));
    ASSERT_TRUE(node.rename_database("warehouse", "lake"));

    zetta::ml::ModelValue value;
    value.kind = "classifier";
    auto model = node.create_model(value);
    ASSERT_TRUE(model);
    const std::string model_id = model->id();

    const auto run = node.run_graph(sample_graph());
    ASSERT_TRUE(run.run);

    ASSERT_TRUE(node.drop_model(model_id));
    ASSERT_TRUE(node.drop_database("lake"));
    ASSERT_TRUE(node.drop_project("retention"));

    EXPECT_EQ(rows(node, AuditAction::DataChanged, "project:churn").size(), 1u);
    EXPECT_EQ(rows(node, AuditAction::DataChanged, "project:retention").size(), 2u);
    for (const auto& row : rows(node, AuditAction::DataChanged, "project:retention")) {
        EXPECT_EQ(row->actor(), "local");
    }
    EXPECT_EQ(expect_one(node, AuditAction::DataChanged, "database:warehouse", "local"),
              "database created");
    EXPECT_EQ(rows(node, AuditAction::DataChanged, "database:lake").size(), 2u);
    EXPECT_EQ(rows(node, AuditAction::DataChanged, "model:" + model_id).size(), 2u);
    EXPECT_NE(expect_one(node, AuditAction::DataChanged, "run:" + run.run->id(), "local")
                  .find("graph run"),
              std::string::npos);

    // A no-op change (nothing to drop, a name clash) records nothing new.
    const std::size_t before = node.audit_event_count();
    EXPECT_FALSE(node.drop_project("retention"));
    EXPECT_FALSE(node.drop_database("lake"));
    EXPECT_FALSE(node.create_database("system"));
    EXPECT_EQ(node.audit_event_count(), before);
    node.stop();
}

// Deploys and queued jobs used to record an empty actor, and graph changes named
// the project's owner whoever made them.
TEST(AuditCompleteness, DeploysJobsAndGraphChangesNameTheCaller) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    auto project = node.create_project("churn", "owner-key");
    ASSERT_TRUE(project);

    const AuditActorScope caller{std::string("user-bo")};
    ASSERT_TRUE(node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn"));
    auto job = node.create_job(zetta::compute::JobKind::Train);
    ASSERT_TRUE(job);
    ASSERT_TRUE(project->create_graph("pipeline", sample_graph()));
    ASSERT_TRUE(project->drop_graph("pipeline"));

    expect_one(node, AuditAction::GraphDeployed, "churn", "user-bo");
    expect_one(node, AuditAction::JobCreated, job->id(), "user-bo");
    expect_one(node, AuditAction::GraphCreated, "pipeline", "user-bo");
    expect_one(node, AuditAction::GraphDropped, "pipeline", "user-bo");
    node.stop();
}

// With no caller declared (a peer, a bare library call) a graph change still
// falls back to the project's owner, as before.
TEST(AuditCompleteness, AGraphChangeWithNoDeclaredCallerNamesTheOwner) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    auto project = node.create_project("churn", "owner-key");
    ASSERT_TRUE(project);
    ASSERT_TRUE(project->create_graph("pipeline", sample_graph()));
    expect_one(node, AuditAction::GraphCreated, "pipeline", "owner-key");
    node.stop();
}

// Work handed to the owner thread carries the caller with it: the owner thread
// is busy as someone else (the desktop frame) when it runs the work.
TEST(AuditCompleteness, RunOnOwnerCarriesTheCallerToTheOwnerThread) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    node.tick();  // adopt this thread as the owner

    std::atomic<bool> done{false};
    std::thread worker([&] {
        const AuditActorScope caller{std::string("user-cy")};
        node.run_on_owner([&] { node.create_database("from_worker"); });
        done = true;
    });
    while (!done) {
        const AuditActorScope desk{std::string("local")};
        node.drain_owner_work();
        std::this_thread::yield();
    }
    worker.join();
    expect_one(node, AuditAction::DataChanged, "database:from_worker", "user-cy");
    node.stop();
}

// Over a capability: the Node's own row names the calling user, and a call that
// was authorized and then failed is on the trail as a failure — before #847 the
// trail held only the pre-execution "action call" row, identical to a success.
TEST(AuditCompleteness, CapabilityCallsAttributeTheirEffectsAndRecordFailures) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    node.ensure_builtin_roles();
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    auto user = node.create_user("ada");
    ASSERT_TRUE(user);
    node.assign_role(user->id(), node.find_role("admin")->id());

    const auto trace = zetta::observability::make_root_trace();
    const auto call = [&](const std::string& name, nlohmann::json input) {
        zetta::api::ApiContext context{node, nullptr, user->id(), trace,
                                       &node.span_recorder()};
        context.acting_is_user = true;
        return registry.call(name, input, context);
    };

    ASSERT_TRUE(call("create_project", {{"name", "churn"}}).ok);
    expect_one(node, AuditAction::DataChanged, "project:churn", user->id());

    const auto clash = call("create_project", {{"name", "churn"}});
    ASSERT_FALSE(clash.ok);
    const auto calls = rows(node, AuditAction::DataAccess, "api:create_project");
    ASSERT_EQ(calls.size(), 3u);  // two authorizations, then the failure last
    EXPECT_EQ(calls.back()->actor(), user->id());
    EXPECT_TRUE(calls.back()->allowed());  // authorized; it failed, it was not refused
    EXPECT_EQ(calls.back()->detail().rfind("failed: engine_error", 0), 0u)
        << calls.back()->detail();
    node.stop();
}

// SQL: the database a statement creates is attributed to the session that ran
// it, alongside the statement's own row.
TEST(AuditCompleteness, SqlEffectsNameTheStatementsSession) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    node.ensure_builtin_roles();
    auto user = node.create_user("dee");
    ASSERT_TRUE(user);
    node.assign_role(user->id(), node.find_role("admin")->id());

    zetta::query::QueryService local{node};
    const auto result =
        local.acting_as(user->id(), /*acting_is_user=*/true).run("CREATE DATABASE app;", "");
    ASSERT_TRUE(result.ok()) << result.error;
    expect_one(node, AuditAction::DataChanged, "database:app", user->id());
    node.stop();
}
