// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "cases.hpp"
#include "zetta/governance/access_key.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/usecases/churn/entity_resolution.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/intent_compiler.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/calibration.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/pipeline/validation.hpp"

// The flagship case: the whole UC-1 churn pipeline, gated + governed (the #399
// end-to-end made interactive). synthetic data -> featurize (point-in-time,
// entity-resolved) -> train (GBDT) -> explain (TreeSHAP) -> ranked at-risk +
// drivers -> playbook, through the ONE public churn runner
// (zetta::usecases::churn::run_churn_solution_on_node, #637) over the node-owned
// JobCoordinator (the #579 spine), with the correctness gate passing first and the
// run governed under key-scoped access + the audit trail.
namespace zetta::testkit {
namespace {

using compute::CpuDevice;

std::string number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    return buffer;
}

zetta::usecases::churn::ChurnDataset dataset_for(const TestContext& context, TestResult& note_into) {
    if (!context.custom_data_path.empty()) {
        const zetta::usecases::churn::ChurnDatasetResult loaded = zetta::usecases::churn::load_churn_dataset(context.custom_data_path);
        if (loaded.ok()) {
            note_into.note("using custom dataset at " + context.custom_data_path);
            return loaded.dataset;
        }
        note_into.note("custom dataset failed to load (" + loaded.error +
                       "); using the built-in synthetic dataset");
    }
    return sample_churn_dataset();
}

TestResult run_uc1(const TestOptions& options, TestContext& context) {
    TestResult result = TestResult::pass("");
    const zetta::usecases::churn::ChurnDataset dataset = dataset_for(context, result);
    const bool brand = options.get_choice("resolver", 0) == 1;
    const zetta::usecases::churn::ResolutionStrategy strategy =
        brand ? zetta::usecases::churn::ResolutionStrategy::Brand : zetta::usecases::churn::ResolutionStrategy::FullName;

    // The churn graph + entity-resolution annotation on the clean step.
    const auto compiled = zetta::pipeline::compile_intent("reduce churn and improve retention");
    if (!compiled) return TestResult::fail("could not compile the churn graph");
    ir::ExecutionGraph graph = compiled->graph;
    if (auto clean = graph.find_node("build_features")) {
        clean->attributes["entity_resolution"] =
            entity_resolution_attribute(dataset, strategy).at("entity_resolution");
    }

    // RULE-1: the gate must pass before anything is placed.
    context.emit("running the correctness gate");
    const zetta::pipeline::ValidationReport report = zetta::pipeline::default_gate().run(graph);
    if (brand) {
        // The conflating resolver must block the gate — the governed-correct outcome
        // is that the pipeline never runs.
        if (report.passed() || !report.block_deploy) {
            return TestResult::fail("the conflating resolver should have blocked the gate");
        }
        result.outcome = TestOutcome::Pass;
        result.summary =
            "the Brand resolver conflated the Contoso siblings; the gate blocked the run (RULE-1)";
        result.metric("gate", "blocked").metric("pipeline", "not run");
        return result;
    }
    if (!report.passed() || report.block_deploy) {
        return TestResult::fail("the gate blocked a division-correct graph");
    }

    // The governed node + a scoped, capability-bearing pipeline key. The node owns
    // its compute plane (attach_cluster brings up node.job_coordinator(), the #579
    // spine) so the run goes through the ONE public churn runner (#637).
    LoopbackCluster cluster(1);
    Node& node = cluster.node(0);
    node.attach_cluster(cluster.cluster(0), /*worker_count=*/2);

    auto principal = node.create_principal("retention-pipeline", zetta::governance::PrincipalKind::Service);
    if (!principal) return TestResult::fail("no principal");
    zetta::governance::Scope scope;
    scope.grant(zetta::governance::ResourceKind::Project, "churn").grant(zetta::governance::ResourceKind::Database, "churn");
    zetta::governance::CapabilitySet capabilities;
    capabilities.grant(zetta::governance::CapabilityDomain::Orchestration, "start_run")
        .grant(zetta::governance::CapabilityDomain::Data, "read");
    auto key = principal->create_key(scope, capabilities);
    if (!key) return TestResult::fail("no key");

    const bool in_scope = node.authorize(key->id(), zetta::governance::Resource{zetta::governance::ResourceKind::Project, "churn"}).allowed;
    const bool out_of_scope = node.authorize(key->id(), zetta::governance::Resource{zetta::governance::ResourceKind::Project, "fraud"}).allowed;
    const bool can_run = node.authorize(key->id(), zetta::governance::Operation{zetta::governance::CapabilityDomain::Orchestration, "start_run"}).allowed;
    if (!in_scope || out_of_scope || !can_run) {
        return TestResult::fail("the key-scoped access decisions were wrong");
    }
    node.deploy_graph(graph, zetta::pipeline::DeploymentTarget::local(), "churn");  // governed deploy

    // Run the real pipeline through the canonical runner over the node's live
    // coordinator (encode + stores + featurize spec are built inside it).
    context.emit("running featurize -> train -> explain -> rank -> playbook");
    CpuDevice device;

    zetta::usecases::churn::ChurnSolutionOptions solution_options;
    solution_options.params = churn_gbdt_params(static_cast<int>(options.get_int("trees", 30)),
                                                static_cast<int>(options.get_int("depth", 3)), 16, 0.3);
    solution_options.calibration.enabled = options.get_bool("calibrate", false);
    solution_options.calibration.method = ml::CalibrationMethod::Platt;
    solution_options.top_k = static_cast<std::size_t>(std::max<long>(0, options.get_int("top_k", 0)));
    solution_options.model_name = "churn_model";
    solution_options.project = "churn";
    solution_options.graph = "churn-graph";

    const zetta::usecases::churn::ChurnSolutionResult run =
        zetta::usecases::churn::run_churn_solution_on_node(node, device, graph, dataset,
                                                           solution_options);

    TestResult outcome = TestResult::fail("");  // filled below
    do {
        if (!run.ok() || !run.execution.run) { outcome = TestResult::fail("the run failed: " + run.execution.error); break; }
        if (run.execution.run->status() != zetta::pipeline::RunStatus::Succeeded) { outcome = TestResult::fail("run did not succeed"); break; }
        std::string schema_error;
        if (!zetta::pipeline::validate_run_value(run.execution.run->metadata(), &schema_error)) {
            outcome = TestResult::fail("run is not schema-valid: " + schema_error); break;
        }
        if (run.model_id.empty() || !node.find_model(run.model_id)) {
            outcome = TestResult::fail("no real Model was registered"); break;
        }
        const std::vector<ml::EntityScore>& ranked = run.ranked;
        const std::size_t expected = solution_options.top_k == 0
                                         ? dataset.accounts.size()
                                         : std::min(solution_options.top_k, dataset.accounts.size());
        if (ranked.size() != expected) { outcome = TestResult::fail("ranked list has the wrong size"); break; }
        bool ranked_ok = !ranked.empty();
        for (std::size_t i = 0; i < ranked.size(); ++i) {
            if (ranked[i].risk < 0.0 || ranked[i].risk > 1.0) ranked_ok = false;
            if (i > 0 && ranked[i - 1].risk < ranked[i].risk) ranked_ok = false;
        }
        if (!ranked_ok) { outcome = TestResult::fail("the at-risk list is not a calibrated descending ranking"); break; }
        if (run.top_driver.empty() || run.drivers.empty()) {
            outcome = TestResult::fail("no churn drivers were produced"); break;
        }
        if (run.playbook.size() != ranked.size()) { outcome = TestResult::fail("the playbook does not cover the at-risk list"); break; }

        int succeeded = 0, jobs = 0, job_created = 0, job_finished = 0, data_access = 0, deployed = 0;
        for (const auto& job : node.jobs()) {
            ++jobs;
            if (job->status() == zetta::compute::JobStatus::Succeeded) ++succeeded;
        }
        for (const auto& event : node.audit_events()) {
            switch (event->action()) {
                case zetta::governance::AuditAction::JobCreated: ++job_created; break;
                case zetta::governance::AuditAction::JobFinished: ++job_finished; break;
                case zetta::governance::AuditAction::DataAccess: ++data_access; break;
                case zetta::governance::AuditAction::GraphDeployed: ++deployed; break;
                default: break;
            }
        }
        if (jobs == 0 || succeeded != jobs) { outcome = TestResult::fail("not every compute job succeeded"); break; }
        if (job_created < jobs || job_finished < jobs || deployed != 1 || data_access < 2) {
            outcome = TestResult::fail("the governed flow was not fully audited"); break;
        }

        outcome = TestResult::pass(
            "the gated + governed churn pipeline produced a calibrated ranked list, "
            "drivers, and a playbook");
        outcome.metric("accounts", std::to_string(ranked.size()))
            .metric("top risk", number(ranked.front().risk))
            .metric("top driver", run.top_driver)
            .metric("compute jobs", std::to_string(succeeded) + "/" + std::to_string(jobs) + " succeeded")
            .metric("audit events", std::to_string(node.audit_event_count()))
            .metric("calibrated", solution_options.calibration.enabled ? "yes" : "predict_proba");
        for (const std::string& line : result.log) outcome.note(line);  // carry custom-data note
    } while (false);

    return outcome;
}

}  // namespace

void register_pipeline_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "pipeline.uc1_end_to_end",
        .category = TestCategory::Pipeline,
        .name = "UC-1 churn end-to-end (gated + governed)",
        .description =
            "The whole UC-1 pipeline: synthetic data -> featurize (point-in-time, "
            "entity-resolved) -> train GBDT -> explain (TreeSHAP) -> ranked at-risk "
            "+ drivers -> playbook, through the canonical churn runner over the "
            "node-owned coordinator, with the "
            "correctness gate passing first and the run governed under key-scoped "
            "access + the audit trail. Choose the Brand resolver to watch the gate "
            "block the run (RULE-1). Distributed equivalence is in the Distributed "
            "cases.",
        .options =
            {TestOption::choice("resolver", "Resolver",
                                {"full_name (runs)", "brand (blocks the gate)"}, 0),
             TestOption::integer("trees", "GBDT trees", 30, 1, 300),
             TestOption::integer("depth", "GBDT max depth", 3, 1, 10),
             TestOption::boolean("calibrate", "Calibrate probabilities", false),
             TestOption::integer("top_k", "At-risk list cap (0 = all)", 0, 0, 1000)},
        .run = run_uc1,
    });
}

}  // namespace zetta::testkit
