#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "cases.hpp"
#include "zetta/governance/access_key.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/cloud/azure_provisioner.hpp"
#include "zetta/cloud/burst_identity.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/usecases/churn/entity_resolution.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/intent_compiler.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/pipeline/validation.hpp"

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

zetta::governance::Resource project_resource(std::string name) {
    return zetta::governance::Resource{zetta::governance::ResourceKind::Project, std::move(name)};
}

// Key-scoped access: a request inherits exactly its key's scope + capabilities;
// everything else is denied.
TestResult run_access_control(const TestOptions&, TestContext& context) {
    ScratchDir dir;
    Node node{scratch_config(dir)};
    node.start();
    context.emit("issuing a key scoped to the churn project");

    auto principal = node.create_principal("analyst", zetta::governance::PrincipalKind::User);
    if (!principal) return TestResult::fail("could not create the principal");
    zetta::governance::Scope scope;
    scope.grant(zetta::governance::ResourceKind::Project, "churn");
    zetta::governance::CapabilitySet capabilities;
    capabilities.grant(zetta::governance::CapabilityDomain::Orchestration, "start_run");
    auto key = principal->create_key(scope, capabilities);
    if (!key) return TestResult::fail("could not issue the key");

    const bool in_scope = node.authorize(key->id(), project_resource("churn")).allowed;
    const bool out_of_scope = node.authorize(key->id(), project_resource("fraud")).allowed;
    const bool right_granted =
        node.authorize(key->id(), zetta::governance::Operation{zetta::governance::CapabilityDomain::Orchestration, "start_run"}).allowed;
    const bool right_missing =
        node.authorize(key->id(), zetta::governance::Operation{zetta::governance::CapabilityDomain::Api, "deploy"}).allowed;
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

    const auto compiled = zetta::pipeline::compile_intent("reduce churn");
    if (!compiled) return TestResult::fail("could not compile the churn graph");
    context.emit("deploying a graph + authorizing access");
    if (!node.deploy_graph(compiled->graph, zetta::pipeline::DeploymentTarget::local(), "churn")) {
        return TestResult::fail("deploy_graph was rejected");
    }
    auto principal = node.create_principal("svc", zetta::governance::PrincipalKind::Service);
    if (!principal) return TestResult::fail("could not create the principal");
    zetta::governance::Scope scope;
    scope.grant(zetta::governance::ResourceKind::Database, "churn");
    auto key = principal->create_key(scope);
    if (!key) return TestResult::fail("could not issue the key");
    node.authorize(key->id(), zetta::governance::Resource{zetta::governance::ResourceKind::Database, "churn"});  // allow
    node.authorize(key->id(), zetta::governance::Resource{zetta::governance::ResourceKind::Database, "fraud"});  // deny

    int deployed = 0, data_access = 0;
    for (const auto& event : node.audit_events()) {
        if (event->action() == zetta::governance::AuditAction::GraphDeployed) ++deployed;
        if (event->action() == zetta::governance::AuditAction::DataAccess) ++data_access;
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
// coarse (Brand) resolver conflates the Contoso siblings and is blocked (TC-4 / RULE-1).
TestResult run_entity_resolution_gate(const TestOptions& options, TestContext& context) {
    const bool brand = options.get_choice("resolver", 0) == 1;
    const zetta::usecases::churn::ResolutionStrategy strategy =
        brand ? zetta::usecases::churn::ResolutionStrategy::Brand : zetta::usecases::churn::ResolutionStrategy::FullName;
    context.emit(brand ? "resolving with the coarse Brand strategy (the trap)"
                       : "resolving division-correct (FullName)");

    const zetta::usecases::churn::ChurnDataset dataset = sample_churn_dataset();
    const auto compiled = zetta::pipeline::compile_intent("reduce churn and improve retention");
    if (!compiled) return TestResult::fail("could not compile the churn graph");
    ir::ExecutionGraph graph = compiled->graph;
    auto clean = graph.find_node("build_features");
    if (!clean) return TestResult::fail("the churn skeleton has no clean step");
    clean->attributes["entity_resolution"] =
        entity_resolution_attribute(dataset, strategy).at("entity_resolution");

    const zetta::pipeline::ValidationReport report = zetta::pipeline::default_gate().run(graph);
    zetta::pipeline::CheckStatus resolution_status = zetta::pipeline::CheckStatus::Pass;
    for (const zetta::pipeline::CheckResult& check : report.checks) {
        if (check.name == "entity_resolution") resolution_status = check.status;
    }

    if (brand) {
        if (report.passed() || !report.block_deploy ||
            resolution_status != zetta::pipeline::CheckStatus::Fail) {
            return TestResult::fail("the conflating resolver should have blocked the gate");
        }
        TestResult result = TestResult::pass(
            "the Brand resolver conflated the Contoso siblings and the gate blocked deploy");
        result.metric("gate", "blocked")
            .metric("entity_resolution", "fail");
        return result;
    }
    if (!report.passed() || report.block_deploy ||
        resolution_status != zetta::pipeline::CheckStatus::Pass) {
        return TestResult::fail("the division-correct resolver should have passed the gate");
    }
    TestResult result = TestResult::pass(
        "the Contoso siblings stayed separate accounts and the gate passed");
    result.metric("gate", "passed").metric("entity_resolution", "pass");
    return result;
}

// A fake AzureControlPlane modelling a burst pool: bring_up scales the pool to the
// requested count, destroy removes one node, list reports what is up. It lets the
// burst-identity lifecycle run against the **real** AzureProvisioner (the reference
// ADR-20 ephemeral-burst backend) with no cloud SDK / CLI.
class FakeBurstControlPlane : public zetta::cloud::AzureControlPlane {
public:
    zetta::cloud::ProvisionResult bring_up(const zetta::cloud::AzureConfig&,
                             const zetta::cloud::ProvisionRequest& request) override {
        pool_.clear();
        for (std::size_t index = 0; index < request.count; ++index) {
            zetta::cloud::ProvisionedNode node;
            node.node_id = "burst-" + std::to_string(index);
            node.address = "10.9.0." + std::to_string(index + 1) + ":7777";
            node.capability = request.spec;
            pool_.push_back(node);
        }
        zetta::cloud::ProvisionResult result;
        result.nodes = pool_;
        result.satisfied = pool_.size() == request.count;
        result.detail = "scaled";
        return result;
    }

    bool destroy(const zetta::cloud::AzureConfig&, const std::string& node_id) override {
        const auto found = std::find_if(
            pool_.begin(), pool_.end(),
            [&](const zetta::cloud::ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == pool_.end()) return false;
        pool_.erase(found);
        return true;
    }

    std::vector<zetta::cloud::ProvisionedNode> list(const zetta::cloud::AzureConfig&) const override {
        return pool_;
    }

private:
    std::vector<zetta::cloud::ProvisionedNode> pool_;
};

std::size_t count_audit(const Node& node, zetta::governance::AuditAction action) {
    std::size_t total = 0;
    for (const auto& event : node.audit_events()) {
        if (event->action() == action) ++total;
    }
    return total;
}

// TC-6 — Bulk burst identity + teardown revocation (ADR-20 / RSK-9). Burst N nodes
// through the **real** AzureProvisioner wired to a **real** BurstIdentityManager via
// the IdentityHooks: provisioning mints N **distinct** short-lived service
// identities (so the burst nodes can authenticate + join the cluster), and tearing
// the burst down revokes them in bulk with **no orphans** left behind. Both edges
// are audited (key_issued on mint, key_revoked on revoke).
TestResult run_burst_identity(const TestOptions& options, TestContext& context) {
    const auto count = static_cast<std::size_t>(std::max<long>(1, options.get_int("nodes", 3)));

    ScratchDir dir;
    Node node{scratch_config(dir)};
    node.start();

    FakeBurstControlPlane control_plane;
    zetta::cloud::BurstIdentityManager identities{node};
    const std::string job = "job-churn-burst";

    // Scope each burst key to the job's project + a TTL'd orchestration right, so the
    // identity is both narrow (only its job) and short-lived (expires with the burst).
    zetta::governance::Scope scope;
    scope.grant(zetta::governance::ResourceKind::Project, "churn");
    zetta::governance::CapabilitySet capabilities;
    capabilities.grant(zetta::governance::CapabilityDomain::Orchestration, "run",
                       "2999-01-01T00:00:00.000Z");

    // Bind the burst provisioner's identity hooks to the real manager: mint on
    // provision, bulk-revoke on the teardown that drains the pool.
    zetta::cloud::AzureProvisioner::IdentityHooks hooks;
    hooks.mint = [&](const std::vector<zetta::cloud::ProvisionedNode>& nodes) {
        identities.mint(job, nodes, scope, capabilities);
    };
    hooks.revoke = [&] { identities.revoke(job); };

    zetta::cloud::AzureConfig config;
    config.subscription_id = "sub-test";
    config.resource_group = "zetta-rg";
    config.location = "eastus";
    zetta::cloud::AzureProvisioner provisioner{control_plane, config, hooks};

    // ---- burst N nodes -> mint N distinct identities --------------------------
    context.emit("bursting " + std::to_string(count) + " nodes");
    zetta::cloud::NodeCapability spec;
    spec.cores = 8;
    const zetta::cloud::ProvisionResult burst =
        provisioner.provision(zetta::cloud::ProvisionRequest{count, spec});
    if (!burst.satisfied || burst.nodes.size() != count) {
        node.stop();
        return TestResult::fail("the burst did not bring up the requested nodes");
    }

    const std::vector<zetta::cloud::BurstIdentity> minted = identities.minted(job);
    if (minted.size() != count) {
        node.stop();
        return TestResult::fail("provision did not mint one identity per burst node");
    }

    // The N identities are distinct: distinct nodes, distinct principals, distinct
    // keys (no two burst nodes share a credential).
    std::set<std::string> node_ids, principals, keys;
    for (const zetta::cloud::BurstIdentity& identity : minted) {
        node_ids.insert(identity.node_id);
        principals.insert(identity.principal_id);
        keys.insert(identity.key_id);
    }
    if (node_ids.size() != count || principals.size() != count || keys.size() != count) {
        node.stop();
        return TestResult::fail("the minted identities were not all distinct");
    }
    if (node.principal_count() != count ||
        count_audit(node, zetta::governance::AuditAction::KeyIssued) != count) {
        node.stop();
        return TestResult::fail("expected N service principals + N key_issued events");
    }

    // ---- tear the burst down -> revoke with no orphans ------------------------
    context.emit("tearing the burst down");
    std::vector<std::string> live_ids;
    for (const zetta::cloud::ProvisionedNode& live : provisioner.provisioned()) {
        live_ids.push_back(live.node_id);
    }
    for (const std::string& id : live_ids) provisioner.teardown(id);

    const std::size_t orphan_principals = node.principal_count();
    const std::size_t still_minted = identities.minted(job).size();
    const std::size_t revoked_events = count_audit(node, zetta::governance::AuditAction::KeyRevoked);
    node.stop();

    if (orphan_principals != 0 || still_minted != 0) {
        return TestResult::fail("teardown left orphan identities behind");
    }
    if (revoked_events != count) {
        return TestResult::fail("expected N key_revoked audit events on teardown");
    }

    TestResult result = TestResult::pass(
        "burst of " + std::to_string(count) +
        " minted N distinct identities; teardown revoked them with no orphans");
    result.metric("burst nodes", std::to_string(count))
        .metric("distinct principals", std::to_string(principals.size()))
        .metric("distinct keys", std::to_string(keys.size()))
        .metric("key_issued", std::to_string(count))
        .metric("key_revoked", std::to_string(revoked_events))
        .metric("orphans", "0");
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
                       "division-correct and passes; Brand conflates the Contoso "
                       "siblings and is blocked (RULE-1).",
        .options = {TestOption::choice("resolver", "Resolver",
                                       {"full_name (correct)", "brand (conflates)"}, 0,
                                       "which resolution strategy produced the annotation")},
        .run = run_entity_resolution_gate,
    });
    suite.add(TestCase{
        .id = "governance.burst_identity",
        .category = TestCategory::Governance,
        .name = "Burst identity + teardown (TC-6)",
        .description = "Burst N nodes through the real cloud-burst provisioner wired "
                       "to a BurstIdentityManager: provisioning mints N distinct "
                       "short-lived service identities, and tearing the burst down "
                       "revokes them in bulk with no orphans (ADR-20 / RSK-9). Both "
                       "edges are audited (key_issued / key_revoked).",
        .options = {TestOption::integer("nodes", "Burst size", 3, 1, 16,
                                        "how many nodes to burst (one identity each)")},
        .run = run_burst_identity,
    });
}

}  // namespace zetta::testkit
