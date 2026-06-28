#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/access_key.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/capability.hpp"
#include "zetta/churn_dataset.hpp"
#include "zetta/churn_solution.hpp"
#include "zetta/churn_step_executor.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/deployment_target.hpp"
#include "zetta/executor.hpp"
#include "zetta/gate.hpp"
#include "zetta/intent_compiler.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/model.hpp"
#include "zetta/node.hpp"
#include "zetta/principal.hpp"
#include "zetta/run.hpp"
#include "zetta/scope.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/validation.hpp"

// UC-1 end-to-end (gated + governed) — issue #399, realizing UC-1 / satisfying
// TP-1. The make-or-break integration test: it drives the whole churn pipeline on
// the committed synthetic fixture and asserts the three acceptance properties at
// once —
//
//   (1) a CALIBRATED RANKED at-risk list (highest risk first, every risk a
//       probability in [0, 1]) + per-entity churn drivers;
//   (2) DIVISION-CORRECT entity resolution — "FedEx Freight" and its 5 sibling
//       subsidiaries stay separate accounts — verified through the correctness
//       gate's EntityResolutionCheck (RULE-1), and a coarse (conflating) resolver
//       is shown to BLOCK the gate;
//   (3) a GATED, GOVERNED run — the gate passes before anything is placed, the
//       pipeline authorizes against a scoped, capability-bearing key (key-scoped
//       access, RULE-3 / ENT-22), and every governed action (data access, deploy,
//       and each unit of accelerated compute) lands in the immutable audit trail.
//
// synthetic data -> featurize (point-in-time, entity-resolved) -> train
// (distributed GBDT) -> explain (TreeSHAP) -> ranked at-risk + drivers -> playbook.
//
// The pipeline compute runs behind the real ChurnStepExecutor seam (#396), the
// same harness the per-op job suites use; single-node here (the underlying jobs'
// single-node == multi-node equivalence is proven in the per-op test suites and the
// native oracle). This test composes the already-built pieces (real StepExecutors,
// the playbook, entity resolution, the oracle parity tests) into the one governed,
// gated end-to-end UC-1 asks for.
namespace {

namespace fs = std::filesystem;

using zetta::AuditAction;
using zetta::CapabilityDomain;
using zetta::CapabilitySet;
using zetta::CheckResult;
using zetta::CheckStatus;
using zetta::ChurnDataset;
using zetta::ChurnPipelineConfig;
using zetta::ChurnStepExecutor;
using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::CorrectnessGate;
using zetta::ExecutionResult;
using zetta::GraphExecutor;
using zetta::IntentShape;
using zetta::JobKind;
using zetta::JobStatus;
using zetta::JsonStore;
using zetta::Model;
using zetta::Node;
using zetta::NodeConfig;
using zetta::Operation;
using zetta::PrincipalKind;
using zetta::Resource;
using zetta::ResourceKind;
using zetta::ResolutionStrategy;
using zetta::RetentionAction;
using zetta::RunOutput;
using zetta::RunStatus;
using zetta::Scope;
using zetta::ValidationReport;
using zetta::compute::CpuDevice;
using zetta::compute::JobCoordinator;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::FeaturizeSpec;
using zetta::ddf::WindowedFeature;
using zetta::ml::EntityScore;
using zetta::ml::GbdtParams;
using zetta::test::TempDir;
using DataType = zetta::compute::DataType;
using Json = nlohmann::json;

// --- The churn fixture, encoded to numeric DDF input shards ------------------
// The encoding + the standard featurize spec / GBDT params are the canonical
// zetta::churn_solution.hpp surface (encode_churn_dataset, churn_events_schema /
// churn_accounts_schema, churn_featurize_spec, default_churn_gbdt_params) — the
// same the GBDT / TreeSHAP / ChurnStepExecutor suites share.

JsonStore make_store(const std::vector<Json>& rows) {
    JsonStore store;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        store.add("row-" + std::to_string(index), rows[index]);
    }
    return store;
}

// A single-node loopback compute node: a Node + a one-node Cluster + a started
// JobCoordinator over it (the harness the per-op job suites use).
struct ComputeNode {
    explicit ComputeNode(std::uint16_t port)
        : node(node_config(temp_dir)),
          cluster("uc1-node", cluster_config(port)),
          coordinator(cluster, /*worker_count=*/2) {
        node.start();
        cluster.start();
        coordinator.start();
    }
    ~ComputeNode() { coordinator.stop(); }
    static NodeConfig node_config(const TempDir& dir) {
        NodeConfig cfg;
        cfg.data_dir = dir.path().string();
        return cfg;
    }
    static ClusterConfig cluster_config(std::uint16_t port) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        return config;
    }
    TempDir temp_dir;  // declared first so it outlives `node`
    Node node;
    Cluster cluster;
    JobCoordinator coordinator;
};

// The churn ExecutionGraph (the intent compiler's churn skeleton) with the
// dataset's resolved-from-the-mentions entity_resolution annotation attached to
// the clean / feature-build step, so the gate's EntityResolutionCheck verifies the
// resolution. `strategy` picks the resolver: FullName is division-correct (passes
// the gate); Brand conflates the FedEx siblings (the TC-4 trap that fails it).
zetta::ir::ExecutionGraph churn_graph_with_resolution(const ChurnDataset& dataset,
                                                      ResolutionStrategy strategy) {
    auto compiled = zetta::compile_intent("reduce churn and improve retention");
    zetta::ir::ExecutionGraph graph = compiled ? compiled->graph
                                               : zetta::ir::ExecutionGraph{};
    if (auto clean = graph.find_node("build_features")) {
        clean->attributes["entity_resolution"] =
            zetta::entity_resolution_attribute(dataset, strategy).at(
                "entity_resolution");
    }
    return graph;
}

// Is `name` present among the report's checks with the given status?
bool check_has_status(const ValidationReport& report, const std::string& name,
                      CheckStatus status) {
    for (const CheckResult& check : report.checks) {
        if (check.name == name) return check.status == status;
    }
    return false;
}

}  // namespace

// =====================================================================
// Acceptance: one integration test drives the whole UC-1 pipeline on the
// fixture and asserts a calibrated ranked list + division-correct resolution +
// a gated, governed run.
// =====================================================================

TEST(Uc1EndToEnd, GatedGovernedRunProducesCalibratedRankedListAndDrivers) {
    // ---- the fixture: synthetic, committed, no foreign runtime ----------------
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::load_churn_dataset(churn_dir);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    const ChurnDataset& dataset = loaded.dataset;

    // ---- the churn graph with division-correct entity resolution attached -----
    const auto compiled = zetta::compile_intent("reduce churn and improve retention");
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->shape, IntentShape::Churn);
    const zetta::ir::ExecutionGraph graph =
        churn_graph_with_resolution(dataset, ResolutionStrategy::FullName);
    ASSERT_TRUE(graph.find_node("build_features"));

    // ---- RULE-1: the correctness gate must pass before anything is placed -----
    const CorrectnessGate gate = zetta::default_gate();
    const ValidationReport report = gate.run(graph);
    EXPECT_TRUE(report.passed());
    EXPECT_FALSE(report.block_deploy);
    // The make-or-break eval ran and PASSED — the FedEx siblings were resolved
    // division-correct (no conflation), so the run is allowed to proceed.
    EXPECT_TRUE(check_has_status(report, "entity_resolution", CheckStatus::Pass));

    // ---- the governed node + a scoped, capability-bearing pipeline key --------
    ComputeNode host(24420);
    Node& node = host.node;

    auto pipeline = node.create_principal("retention-pipeline", PrincipalKind::Service);
    ASSERT_TRUE(pipeline);
    Scope scope;
    scope.grant(ResourceKind::Project, "churn")
        .grant(ResourceKind::Database, "churn");
    CapabilitySet capabilities;
    capabilities.grant(CapabilityDomain::Orchestration, "start_run")
        .grant(CapabilityDomain::Data, "read");
    auto key = pipeline->create_key(scope, capabilities);
    ASSERT_TRUE(key);

    // The run is GOVERNED: it authorizes against its key before acting. The
    // resource decisions each append a data_access audit event (RULE-3 + FEAT-9);
    // the capability decision enforces the orchestration right (ENT-22).
    EXPECT_TRUE(node.authorize(key->id(), Resource{ResourceKind::Project, "churn"})
                    .allowed);  // in scope -> allowed
    EXPECT_TRUE(node.authorize(key->id(),
                               Operation{CapabilityDomain::Orchestration, "start_run"})
                    .allowed);  // holds the start_run capability
    EXPECT_FALSE(node.authorize(key->id(), Resource{ResourceKind::Project, "fraud"})
                     .allowed);  // out of scope -> denied
    EXPECT_FALSE(node.authorize("no-such-key",
                                Resource{ResourceKind::Project, "churn"})
                     .allowed);  // deny-by-default (no credential)
    EXPECT_FALSE(node.authorize(key->id(),
                                Operation{CapabilityDomain::Api, "deploy"})
                     .allowed);  // a right never granted

    // ---- the gate passed + access granted => place the graph (governed) -------
    auto deployment =
        node.deploy_graph(graph, zetta::DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);  // records a graph_deployed audit event

    // ---- run the real UC-1 pipeline behind the StepExecutor seam --------------
    const zetta::ChurnNumericShards numeric = zetta::encode_churn_dataset(dataset);
    const JsonStore events = make_store(numeric.events);
    const JsonStore accounts = make_store(numeric.accounts);
    CpuDevice device;

    ChurnPipelineConfig config;
    config.events_store = &events;
    config.events_schema = zetta::churn_events_schema();
    config.accounts_store = &accounts;
    config.accounts_schema = zetta::churn_accounts_schema();
    config.featurize = zetta::churn_featurize_spec(numeric.as_of);
    config.params = zetta::default_churn_gbdt_params();
    config.entity_column = "entity";
    config.model_name = "churn_model";
    config.project = "churn";
    config.graph = "churn-graph-1";

    auto churn = std::make_shared<ChurnStepExecutor>(node, host.coordinator, device,
                                                     config);
    GraphExecutor executor{churn};
    const ExecutionResult result =
        executor.execute(node, graph, "churn", "churn-graph-1");

    // The run succeeded end to end and is schema-valid.
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.run);
    EXPECT_EQ(result.run->status(), RunStatus::Succeeded);
    EXPECT_TRUE(result.failed_step.empty());
    std::string schema_error;
    EXPECT_TRUE(zetta::validate_run_value(result.run->metadata(), &schema_error))
        << schema_error;

    // (1) A real, registered Model the run's `model` output references.
    const std::vector<RunOutput>& outputs = result.run->outputs();
    const bool has_model_output =
        std::any_of(outputs.begin(), outputs.end(),
                    [](const RunOutput& o) { return o.kind == "model"; });
    EXPECT_TRUE(has_model_output);
    ASSERT_FALSE(churn->model_id().empty());
    std::shared_ptr<Model> registered = node.find_model(churn->model_id());
    ASSERT_TRUE(registered) << "model output ref does not resolve to a real Model";
    EXPECT_EQ(registered->kind(), "classifier");

    // (2) A CALIBRATED, RANKED at-risk list — one per account, highest risk first,
    // every risk a probability in [0, 1] (the binary-logistic predict_proba is the
    // calibrated churn risk; see docs/gbdt-score.md).
    const std::vector<EntityScore>& ranked = churn->ranked();
    ASSERT_FALSE(ranked.empty());
    EXPECT_EQ(ranked.size(), dataset.accounts.size());
    for (const EntityScore& score : ranked) {
        EXPECT_GE(score.risk, 0.0);
        EXPECT_LE(score.risk, 1.0);
    }
    for (std::size_t i = 1; i < ranked.size(); ++i) {
        EXPECT_GE(ranked[i - 1].risk, ranked[i].risk) << "rank " << i;
    }

    // (3) Drivers (exact TreeSHAP) — failed payments is the dominant churn driver.
    EXPECT_EQ(churn->top_driver(), "failed_payments");
    const auto driver_ranking = churn->driver_ranking();
    ASSERT_FALSE(driver_ranking.empty());
    EXPECT_EQ(driver_ranking.front().first, "failed_payments");

    // (4) The retention playbook joins the at-risk list with each entity's driver.
    const std::vector<RetentionAction>& playbook = churn->playbook();
    ASSERT_FALSE(playbook.empty());
    EXPECT_EQ(playbook.size(), ranked.size());
    EXPECT_EQ(playbook.front().entity, ranked.front().entity);
    EXPECT_TRUE(std::any_of(
        playbook.begin(), playbook.end(),
        [](const RetentionAction& a) { return !a.dominant_driver.empty(); }));

    // Every heavy step ran as a recorded ComputeJob, all Succeeded.
    int featurize = 0, train = 0, score = 0, explain = 0;
    for (const auto& job : node.jobs()) {
        EXPECT_EQ(job->status(), JobStatus::Succeeded) << job->id();
        switch (job->kind()) {
            case JobKind::Featurize: ++featurize; break;
            case JobKind::Train:     ++train; break;
            case JobKind::Score:     ++score; break;
            case JobKind::Explain:   ++explain; break;
            case JobKind::Ddf:       break;
        }
    }
    EXPECT_EQ(featurize, 1);
    EXPECT_EQ(train, 1);
    EXPECT_EQ(score, 1);
    EXPECT_EQ(explain, 1);

    // GOVERNED: the immutable audit trail captured the whole flow — the key-scoped
    // access decisions, the deploy, and every unit of accelerated compute
    // (job created + finished). Nothing in the pipeline ran un-audited.
    int data_access = 0, deploy = 0, job_created = 0, job_finished = 0;
    for (const auto& event : node.audit_events()) {
        switch (event->action()) {
            case AuditAction::DataAccess:    ++data_access; break;
            case AuditAction::GraphDeployed: ++deploy; break;
            case AuditAction::JobCreated:    ++job_created; break;
            case AuditAction::JobFinished:   ++job_finished; break;
            default: break;
        }
    }
    EXPECT_GE(data_access, 3);   // the authorize(resource) decisions above
    EXPECT_EQ(deploy, 1);        // the governed deploy
    EXPECT_EQ(job_created, 4);   // featurize / train / score / explain
    EXPECT_EQ(job_finished, 4);  // each reached a terminal (Succeeded) state
}

// The make-or-break negative: a coarse (Brand) resolver conflates the FedEx
// siblings onto one account — the canonical TC-4 failure — so the correctness gate
// FAILS and blocks deploy (RULE-1). The run never happens: no model is trained, no
// compute is placed. This is the teeth behind "division-correct resolution".
TEST(Uc1EndToEnd, ConflatingResolutionBlocksTheGatedRun) {
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::load_churn_dataset(churn_dir);
    ASSERT_TRUE(loaded.ok()) << loaded.error;

    const zetta::ir::ExecutionGraph graph =
        churn_graph_with_resolution(loaded.dataset, ResolutionStrategy::Brand);
    ASSERT_TRUE(graph.find_node("build_features"));

    const ValidationReport report = zetta::default_gate().run(graph);
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.block_deploy);  // RULE-1: conflation blocks deploy
    EXPECT_TRUE(check_has_status(report, "entity_resolution", CheckStatus::Fail));
}
