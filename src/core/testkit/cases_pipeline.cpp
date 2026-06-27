#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "cases.hpp"
#include "zetta/access_key.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/capability.hpp"
#include "zetta/churn_dataset.hpp"
#include "zetta/churn_step_executor.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/deployment_target.hpp"
#include "zetta/entity_resolution.hpp"
#include "zetta/executor.hpp"
#include "zetta/gate.hpp"
#include "zetta/intent_compiler.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/calibration.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/model.hpp"
#include "zetta/node.hpp"
#include "zetta/principal.hpp"
#include "zetta/run.hpp"
#include "zetta/scope.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/validation.hpp"

// The flagship case: the whole UC-1 churn pipeline, gated + governed (the #399
// end-to-end made interactive). synthetic data -> featurize (point-in-time,
// entity-resolved) -> train (GBDT) -> explain (TreeSHAP) -> ranked at-risk +
// drivers -> playbook, behind the real ChurnStepExecutor, with the correctness gate
// passing first and the run governed under key-scoped access + the audit trail.
namespace zetta::testkit {
namespace {

using compute::CpuDevice;
using compute::JobCoordinator;

std::string number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    return buffer;
}

ChurnDataset dataset_for(const TestContext& context, TestResult& note_into) {
    if (!context.custom_data_path.empty()) {
        const ChurnDatasetResult loaded = load_churn_dataset(context.custom_data_path);
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
    const ChurnDataset dataset = dataset_for(context, result);
    const bool brand = options.get_choice("resolver", 0) == 1;
    const ResolutionStrategy strategy =
        brand ? ResolutionStrategy::Brand : ResolutionStrategy::FullName;

    // The churn graph + entity-resolution annotation on the clean step.
    const auto compiled = compile_intent("reduce churn and improve retention");
    if (!compiled) return TestResult::fail("could not compile the churn graph");
    ir::ExecutionGraph graph = compiled->graph;
    if (auto clean = graph.find_node("build_features")) {
        clean->attributes["entity_resolution"] =
            entity_resolution_attribute(dataset, strategy).at("entity_resolution");
    }

    // RULE-1: the gate must pass before anything is placed.
    context.emit("running the correctness gate");
    const ValidationReport report = default_gate().run(graph);
    if (brand) {
        // The conflating resolver must block the gate — the governed-correct outcome
        // is that the pipeline never runs.
        if (report.passed() || !report.block_deploy) {
            return TestResult::fail("the conflating resolver should have blocked the gate");
        }
        result.outcome = TestOutcome::Pass;
        result.summary =
            "the Brand resolver conflated the FedEx siblings; the gate blocked the run (RULE-1)";
        result.metric("gate", "blocked").metric("pipeline", "not run");
        return result;
    }
    if (!report.passed() || report.block_deploy) {
        return TestResult::fail("the gate blocked a division-correct graph");
    }

    // The governed node + a scoped, capability-bearing pipeline key.
    LoopbackCluster cluster(1);
    Node& node = cluster.node(0);
    JobCoordinator coordinator(cluster.cluster(0), /*worker_count=*/2);
    coordinator.start();

    auto principal = node.create_principal("retention-pipeline", PrincipalKind::Service);
    if (!principal) { coordinator.stop(); return TestResult::fail("no principal"); }
    Scope scope;
    scope.grant(ResourceKind::Project, "churn").grant(ResourceKind::Database, "churn");
    CapabilitySet capabilities;
    capabilities.grant(CapabilityDomain::Orchestration, "start_run")
        .grant(CapabilityDomain::Data, "read");
    auto key = principal->create_key(scope, capabilities);
    if (!key) { coordinator.stop(); return TestResult::fail("no key"); }

    const bool in_scope = node.authorize(key->id(), Resource{ResourceKind::Project, "churn"}).allowed;
    const bool out_of_scope = node.authorize(key->id(), Resource{ResourceKind::Project, "fraud"}).allowed;
    const bool can_run = node.authorize(key->id(), Operation{CapabilityDomain::Orchestration, "start_run"}).allowed;
    if (!in_scope || out_of_scope || !can_run) {
        coordinator.stop();
        return TestResult::fail("the key-scoped access decisions were wrong");
    }
    node.deploy_graph(graph, DeploymentTarget::local(), "churn");  // governed deploy

    // Run the real pipeline behind the StepExecutor seam.
    context.emit("running featurize -> train -> explain -> rank -> playbook");
    const ChurnNumeric numeric = encode_churn(dataset);
    const JsonStore events = make_store(numeric.events);
    const JsonStore accounts = make_store(numeric.accounts);
    CpuDevice device;

    ChurnPipelineConfig config;
    config.events_store = &events;
    config.events_schema = churn_events_schema();
    config.accounts_store = &accounts;
    config.accounts_schema = churn_accounts_schema();
    config.featurize = churn_featurize_spec(numeric.as_of);
    config.params = churn_gbdt_params(static_cast<int>(options.get_int("trees", 30)),
                                      static_cast<int>(options.get_int("depth", 3)), 16, 0.3);
    config.calibration.enabled = options.get_bool("calibrate", false);
    config.calibration.method = ml::CalibrationMethod::Platt;
    config.entity_column = "entity";
    config.top_k = static_cast<std::size_t>(std::max<long>(0, options.get_int("top_k", 0)));
    config.model_name = "churn_model";
    config.project = "churn";
    config.graph = "churn-graph";

    auto churn = std::make_shared<ChurnStepExecutor>(node, coordinator, device, config);
    GraphExecutor executor{churn};
    const ExecutionResult run = executor.execute(node, graph, "churn", "churn-graph");

    TestResult outcome = TestResult::fail("");  // filled below
    do {
        if (!run.ok || !run.run) { outcome = TestResult::fail("the run failed: " + run.error); break; }
        if (run.run->status() != RunStatus::Succeeded) { outcome = TestResult::fail("run did not succeed"); break; }
        std::string schema_error;
        if (!validate_run_value(run.run->metadata(), &schema_error)) {
            outcome = TestResult::fail("run is not schema-valid: " + schema_error); break;
        }
        if (churn->model_id().empty() || !node.find_model(churn->model_id())) {
            outcome = TestResult::fail("no real Model was registered"); break;
        }
        const std::vector<ml::EntityScore>& ranked = churn->ranked();
        const std::size_t expected = config.top_k == 0
                                         ? dataset.accounts.size()
                                         : std::min(config.top_k, dataset.accounts.size());
        if (ranked.size() != expected) { outcome = TestResult::fail("ranked list has the wrong size"); break; }
        bool ranked_ok = !ranked.empty();
        for (std::size_t i = 0; i < ranked.size(); ++i) {
            if (ranked[i].risk < 0.0 || ranked[i].risk > 1.0) ranked_ok = false;
            if (i > 0 && ranked[i - 1].risk < ranked[i].risk) ranked_ok = false;
        }
        if (!ranked_ok) { outcome = TestResult::fail("the at-risk list is not a calibrated descending ranking"); break; }
        if (churn->top_driver().empty() || churn->driver_ranking().empty()) {
            outcome = TestResult::fail("no churn drivers were produced"); break;
        }
        if (churn->playbook().size() != ranked.size()) { outcome = TestResult::fail("the playbook does not cover the at-risk list"); break; }

        int succeeded = 0, jobs = 0, job_created = 0, job_finished = 0, data_access = 0, deployed = 0;
        for (const auto& job : node.jobs()) {
            ++jobs;
            if (job->status() == JobStatus::Succeeded) ++succeeded;
        }
        for (const auto& event : node.audit_events()) {
            switch (event->action()) {
                case AuditAction::JobCreated: ++job_created; break;
                case AuditAction::JobFinished: ++job_finished; break;
                case AuditAction::DataAccess: ++data_access; break;
                case AuditAction::GraphDeployed: ++deployed; break;
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
            .metric("top driver", churn->top_driver())
            .metric("compute jobs", std::to_string(succeeded) + "/" + std::to_string(jobs) + " succeeded")
            .metric("audit events", std::to_string(node.audit_event_count()))
            .metric("calibrated", config.calibration.enabled ? "yes" : "predict_proba");
        for (const std::string& line : result.log) outcome.note(line);  // carry custom-data note
    } while (false);

    coordinator.stop();
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
            "+ drivers -> playbook, behind the real ChurnStepExecutor, with the "
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
