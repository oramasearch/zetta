#include <string>
#include <vector>

#include "cases.hpp"
#include "zetta/access_key.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/capability.hpp"
#include "zetta/churn_dataset.hpp"
#include "zetta/deployment_target.hpp"
#include "zetta/entity_resolution.hpp"
#include "zetta/gate.hpp"
#include "zetta/intent_compiler.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/principal.hpp"
#include "zetta/scope.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/validation.hpp"

// Governance cases — the deny-by-default key-scoped access model, the immutable
// audit trail, and the correctness gate's make-or-break entity-resolution eval.
namespace zetta::testkit {
namespace {

NodeConfig scratch_config(const ScratchDir& dir) {
    NodeConfig config;
    config.data_dir = dir.path().string();
    config.preflight = false;
    return config;
}

Resource project_resource(std::string name) {
    return Resource{ResourceKind::Project, std::move(name)};
}

// Key-scoped access: a request inherits exactly its key's scope + capabilities;
// everything else is denied.
TestResult run_access_control(const TestOptions&, TestContext& context) {
    ScratchDir dir;
    Node node{scratch_config(dir)};
    node.start();
    context.emit("issuing a key scoped to the churn project");

    auto principal = node.create_principal("analyst", PrincipalKind::User);
    if (!principal) return TestResult::fail("could not create the principal");
    Scope scope;
    scope.grant(ResourceKind::Project, "churn");
    CapabilitySet capabilities;
    capabilities.grant(CapabilityDomain::Orchestration, "start_run");
    auto key = principal->create_key(scope, capabilities);
    if (!key) return TestResult::fail("could not issue the key");

    const bool in_scope = node.authorize(key->id(), project_resource("churn")).allowed;
    const bool out_of_scope = node.authorize(key->id(), project_resource("fraud")).allowed;
    const bool right_granted =
        node.authorize(key->id(), Operation{CapabilityDomain::Orchestration, "start_run"}).allowed;
    const bool right_missing =
        node.authorize(key->id(), Operation{CapabilityDomain::Api, "deploy"}).allowed;
    const bool unknown_key = node.authorize("no-such-key", project_resource("churn")).allowed;
    node.stop();

    if (!in_scope || out_of_scope || !right_granted || right_missing || unknown_key) {
        return TestResult::fail("the access decisions did not follow deny-by-default");
    }
    TestResult result = TestResult::pass(
        "in-scope + granted right allowed; everything else denied");
    result.metric("in-scope", "allow")
        .metric("out-of-scope", "deny")
        .metric("ungranted right", "deny")
        .metric("unknown key", "deny");
    return result;
}

// The audit trail records the governed actions — a deploy and the access
// decisions — as immutable events.
TestResult run_audit(const TestOptions&, TestContext& context) {
    ScratchDir dir;
    Node node{scratch_config(dir)};
    node.start();

    const auto compiled = compile_intent("reduce churn");
    if (!compiled) return TestResult::fail("could not compile the churn graph");
    context.emit("deploying a graph + authorizing access");
    if (!node.deploy_graph(compiled->graph, DeploymentTarget::local(), "churn")) {
        return TestResult::fail("deploy_graph was rejected");
    }
    auto principal = node.create_principal("svc", PrincipalKind::Service);
    if (!principal) return TestResult::fail("could not create the principal");
    Scope scope;
    scope.grant(ResourceKind::Database, "churn");
    auto key = principal->create_key(scope);
    if (!key) return TestResult::fail("could not issue the key");
    node.authorize(key->id(), Resource{ResourceKind::Database, "churn"});  // allow
    node.authorize(key->id(), Resource{ResourceKind::Database, "fraud"});  // deny

    int deployed = 0, data_access = 0;
    for (const auto& event : node.audit_events()) {
        if (event->action() == AuditAction::GraphDeployed) ++deployed;
        if (event->action() == AuditAction::DataAccess) ++data_access;
    }
    const std::size_t total = node.audit_event_count();
    node.stop();

    if (deployed != 1 || data_access < 2) {
        return TestResult::fail("expected a deploy event + the access decisions in the trail");
    }
    TestResult result = TestResult::pass("the governed actions were all audited");
    result.metric("audit events", std::to_string(total))
        .metric("graph_deployed", std::to_string(deployed))
        .metric("data_access", std::to_string(data_access));
    return result;
}

// The entity-resolution gate: a division-correct (FullName) resolver passes; a
// coarse (Brand) resolver conflates the FedEx siblings and is blocked (TC-4 / RULE-1).
TestResult run_entity_resolution_gate(const TestOptions& options, TestContext& context) {
    const bool brand = options.get_choice("resolver", 0) == 1;
    const ResolutionStrategy strategy =
        brand ? ResolutionStrategy::Brand : ResolutionStrategy::FullName;
    context.emit(brand ? "resolving with the coarse Brand strategy (the trap)"
                       : "resolving division-correct (FullName)");

    const ChurnDataset dataset = sample_churn_dataset();
    const auto compiled = compile_intent("reduce churn and improve retention");
    if (!compiled) return TestResult::fail("could not compile the churn graph");
    ir::ExecutionGraph graph = compiled->graph;
    auto clean = graph.find_node("build_features");
    if (!clean) return TestResult::fail("the churn skeleton has no clean step");
    clean->attributes["entity_resolution"] =
        entity_resolution_attribute(dataset, strategy).at("entity_resolution");

    const ValidationReport report = default_gate().run(graph);
    CheckStatus resolution_status = CheckStatus::Pass;
    for (const CheckResult& check : report.checks) {
        if (check.name == "entity_resolution") resolution_status = check.status;
    }

    if (brand) {
        if (report.passed() || !report.block_deploy ||
            resolution_status != CheckStatus::Fail) {
            return TestResult::fail("the conflating resolver should have blocked the gate");
        }
        TestResult result = TestResult::pass(
            "the Brand resolver conflated the FedEx siblings and the gate blocked deploy");
        result.metric("gate", "blocked")
            .metric("entity_resolution", "fail");
        return result;
    }
    if (!report.passed() || report.block_deploy ||
        resolution_status != CheckStatus::Pass) {
        return TestResult::fail("the division-correct resolver should have passed the gate");
    }
    TestResult result = TestResult::pass(
        "the FedEx siblings stayed separate accounts and the gate passed");
    result.metric("gate", "passed").metric("entity_resolution", "pass");
    return result;
}

}  // namespace

void register_governance_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "governance.access_control",
        .category = TestCategory::Governance,
        .name = "Key-scoped access",
        .description = "Issue a scoped, capability-bearing key and confirm "
                       "deny-by-default: in-scope + granted rights allowed, "
                       "everything else (out-of-scope, ungranted, unknown key) denied.",
        .options = {},
        .run = run_access_control,
    });
    suite.add(TestCase{
        .id = "governance.audit",
        .category = TestCategory::Governance,
        .name = "Audit trail",
        .description = "Deploy a graph and authorize access, then confirm the "
                       "governed actions land in the immutable audit trail.",
        .options = {},
        .run = run_audit,
    });
    suite.add(TestCase{
        .id = "governance.entity_resolution_gate",
        .category = TestCategory::Governance,
        .name = "Entity-resolution gate (TC-4)",
        .description = "Run the correctness gate over a churn graph whose clean "
                       "step carries a resolution annotation. FullName is "
                       "division-correct and passes; Brand conflates the FedEx "
                       "siblings and is blocked (RULE-1).",
        .options = {TestOption::choice("resolver", "Resolver",
                                       {"full_name (correct)", "brand (conflates)"}, 0,
                                       "which resolution strategy produced the annotation")},
        .run = run_entity_resolution_gate,
    });
}

}  // namespace zetta::testkit
