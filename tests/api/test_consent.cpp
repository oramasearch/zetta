// The remote consent challenge protocol (#621; docs/api/consent.md): a
// consent-gated cloud capability NEVER executes on first call — it issues a
// one-time challenge naming exactly what would be read/run — and only a
// distinctly-privileged approval (an approver key of a DIFFERENT principal,
// or the trusted headed surface) executes the frozen action, exactly once.
// The calling key alone can never approve its own cloud action.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cloud/cloud_provider.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/consent.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;
using zetta::governance::ConsentStatus;

// A provisioner that counts provisions — the proof that "does nothing"
// really means nothing, and "exactly once" really means once.
class CountingProvisioner final : public zetta::cloud::Provisioner {
public:
    zetta::cloud::ProvisionResult provision(
        const zetta::cloud::ProvisionRequest& request) override {
        ++provisions;
        zetta::cloud::ProvisionResult result;
        for (std::size_t i = 0; i < request.count; ++i) {
            result.nodes.push_back(
                {"fake-node-" + std::to_string(i), "127.0.0.1:0", {}});
        }
        result.satisfied = true;
        result.detail = "fake burst";
        return result;
    }
    bool teardown(const std::string&) override { return false; }
    std::vector<zetta::cloud::ProvisionedNode> provisioned() const override {
        return {};
    }

    int provisions = 0;
};

struct ConsentFixture : ::testing::Test {
    zetta::test::TempDir dir;
    // Consent enforcement is OFF by default (#995) — every test below is about
    // the protocol that runs when it is ON, so this fixture asks for it
    // explicitly. `ConsentDisabledFixture` at the bottom covers the default.
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false,
                                       /*enforce_consent=*/true}};
    Registry registry;
    zetta::api::BackgroundActions background;
    CountingProvisioner fake_azure;
    zetta::observability::TraceContext trace =
        zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override {
        // Destructors of adopted workers must run on the owner thread.
        wait_for_background();
        node.stop();
    }

    ApiContext context_for(const std::string& acting_key) {
        return ApiContext{node,
                          nullptr,
                          acting_key,
                          trace,
                          &node.span_recorder(),
                          &background,
                          [this](const std::string& target)
                              -> zetta::cloud::Provisioner* {
                              if (target == "azure" || target == "local") {
                                  return &fake_azure;
                              }
                              return nullptr;
                          }};
    }

    ApiResult call(const std::string& name, nlohmann::json input,
                   const std::string& acting_key = {}) {
        ApiContext context = context_for(acting_key);
        return registry.call(name, input, context);
    }

    // Pump the background owner until every adopted worker finished and its
    // owner-thread apply ran — patient, like the multi-node convergence loops.
    void wait_for_background() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (background.pending() > 0 &&
               std::chrono::steady_clock::now() < deadline) {
            background.prune();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        background.prune();
        ASSERT_EQ(background.pending(), 0u) << "background worker never finished";
    }

    // Mint `principal name` -> one key holding the given orchestration
    // capabilities (+ api:query).
    std::string make_key(const std::string& principal_name,
                         const std::vector<std::string>& actions) {
        auto principal = node.create_principal(
            principal_name, zetta::governance::PrincipalKind::Service);
        if (!principal) {
            principal = node.find_principal(principal_name);
        }
        zetta::governance::CapabilitySet grants;
        grants.grant(zetta::governance::CapabilityDomain::Api, "query");
        for (const std::string& action : actions) {
            grants.grant(zetta::governance::CapabilityDomain::Orchestration,
                         action);
        }
        auto key = principal->create_key(zetta::governance::unrestricted_scope(),
                                         std::move(grants));
        return key->id();
    }

    bool audit_contains(zetta::governance::AuditAction action) {
        for (const auto& event : node.audit_events()) {
            if (event->value().action == action) return true;
        }
        return false;
    }
};

// Acceptance 1: a cloud add_nodes over MCP without approval does nothing and
// returns the named consent request.
TEST_F(ConsentFixture, CloudAddNodesWithoutApprovalDoesNothingAndNamesTheAction) {
    auto result = call("add_nodes", {{"target", "azure"}, {"count", 2}});
    ASSERT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "consent_required");

    // The structured challenge names exactly what would be read/run — the
    // Cloud Inspector grammar for the azure catalog entry.
    const nlohmann::json& challenge = result.error.details;
    ASSERT_TRUE(challenge.is_object()) << challenge.dump();
    EXPECT_EQ(challenge.value("capability", ""), "add_nodes");
    EXPECT_EQ(challenge.value("status", ""), "pending");
    EXPECT_FALSE(challenge.value("id", std::string{}).empty());
    const nlohmann::json& action = challenge.at("action");
    EXPECT_NE(action.value("summary", "").find("2 billable nodes"),
              std::string::npos);
    const auto* azure =
        zetta::cloud::CloudProviderCatalog::builtin().find("azure");
    ASSERT_TRUE(azure);
    EXPECT_EQ(action.at("reads").size(), azure->config_paths.size());
    // The challenge JSON honours its embedded schema (the documented contract).
    std::string schema_error;
    EXPECT_TRUE(zetta::governance::validate_consent_challenge(challenge,
                                                              &schema_error))
        << schema_error;

    // Nothing executed, and both the ledger and the audit trail carry the
    // challenge.
    EXPECT_EQ(fake_azure.provisions, 0);
    EXPECT_EQ(node.consent_ledger().pending().size(), 1u);
    EXPECT_TRUE(
        audit_contains(zetta::governance::AuditAction::ConsentRequested));

    // Repeating the call only ever mints another challenge.
    auto repeat = call("add_nodes", {{"target", "azure"}, {"count", 2}});
    EXPECT_FALSE(repeat.ok);
    EXPECT_EQ(fake_azure.provisions, 0);
    EXPECT_EQ(node.consent_ledger().pending().size(), 2u);
}

// Acceptance 3: the calling key alone can never approve its own cloud action —
// not the same key, not a sibling key of the same principal, not the
// unauthenticated local session.
TEST_F(ConsentFixture, TheCallingKeyCanNeverApproveItsOwnAction) {
    // The requester holds EVERY relevant capability — proof that privilege on
    // the calling key is never enough.
    const std::string requester =
        make_key("caller", {"add_nodes", "approve_consent"});
    auto refusal =
        call("add_nodes", {{"target", "azure"}, {"count", 1}}, requester);
    ASSERT_FALSE(refusal.ok);
    const std::string challenge_id = refusal.error.details.value("id", "");
    ASSERT_FALSE(challenge_id.empty());

    // Same key: refused.
    auto self_approval = call("approve_consent",
                              {{"challenge_id", challenge_id}}, requester);
    EXPECT_FALSE(self_approval.ok);
    EXPECT_EQ(self_approval.error.code, "unauthorized");

    // A sibling key of the SAME principal: refused (the different-principal
    // rule, not merely a different-key rule).
    auto sibling = node.find_principal("caller")->create_key(
        zetta::governance::unrestricted_scope(),
        zetta::governance::CapabilitySet{}.grant(
            zetta::governance::CapabilityDomain::Orchestration,
            "approve_consent"));
    auto sibling_approval = call("approve_consent",
                                 {{"challenge_id", challenge_id}}, sibling->id());
    EXPECT_FALSE(sibling_approval.ok);
    EXPECT_EQ(sibling_approval.error.code, "unauthorized");

    // The unauthenticated local session over the wire: refused.
    auto local_approval =
        call("approve_consent", {{"challenge_id", challenge_id}});
    EXPECT_FALSE(local_approval.ok);
    EXPECT_EQ(local_approval.error.code, "unauthorized");

    // A named key WITHOUT orchestration:approve_consent: denied by the
    // ordinary capability check (deny-by-default), before the consent rules.
    const std::string bystander = make_key("bystander", {});
    auto unprivileged = call("approve_consent",
                             {{"challenge_id", challenge_id}}, bystander);
    EXPECT_FALSE(unprivileged.ok);
    EXPECT_EQ(unprivileged.error.code, "unauthorized");

    EXPECT_EQ(fake_azure.provisions, 0);
    EXPECT_EQ(node.consent_ledger().find(challenge_id)->status,
              ConsentStatus::Pending);
}

// Acceptance 2: approval via an approver key (a different principal holding
// orchestration:approve_consent) executes exactly the named action once.
TEST_F(ConsentFixture, ApproverKeyExecutesExactlyTheNamedActionOnce) {
    const std::string requester = make_key("caller", {"add_nodes"});
    auto refusal =
        call("add_nodes", {{"target", "azure"}, {"count", 3}}, requester);
    ASSERT_FALSE(refusal.ok);
    const std::string challenge_id = refusal.error.details.value("id", "");

    const std::string approver = make_key("approver", {"approve_consent"});
    auto approval =
        call("approve_consent", {{"challenge_id", challenge_id}}, approver);
    ASSERT_TRUE(approval.ok) << approval.error.message;
    EXPECT_EQ(approval.value.value("capability", ""), "add_nodes");
    // The provision runs on a background worker; the approval reports it.
    EXPECT_EQ(approval.value.value("status", ""), "executing");

    wait_for_background();
    EXPECT_EQ(fake_azure.provisions, 1);
    auto challenge = node.consent_ledger().find(challenge_id);
    ASSERT_TRUE(challenge);
    EXPECT_EQ(challenge->status, ConsentStatus::Executed);
    EXPECT_EQ(challenge->result.value("satisfied", false), true);
    EXPECT_EQ(challenge->result.at("members").size(), 3u);
    EXPECT_TRUE(
        audit_contains(zetta::governance::AuditAction::ConsentDecided));

    // The one-time-use guarantee: a replayed approval can never run it again.
    auto replay =
        call("approve_consent", {{"challenge_id", challenge_id}}, approver);
    EXPECT_FALSE(replay.ok);
    EXPECT_EQ(fake_azure.provisions, 1);
}

// Acceptance 2 (the other surface): the headed app's approval executes the
// action as the trusted human-in-front-of-the-machine path.
TEST_F(ConsentFixture, HeadedSurfaceApprovalExecutesTheAction) {
    auto refusal = call("add_nodes", {{"target", "azure"}, {"count", 1}});
    ASSERT_FALSE(refusal.ok);
    const std::string challenge_id = refusal.error.details.value("id", "");

    ApiContext context = context_for({});
    auto decided = registry.decide_consent(
        challenge_id, /*approve=*/true, {}, context,
        Registry::ConsentSurface::HeadedApp);
    ASSERT_TRUE(decided.ok) << decided.error.message;

    wait_for_background();
    EXPECT_EQ(fake_azure.provisions, 1);
    EXPECT_EQ(node.consent_ledger().find(challenge_id)->status,
              ConsentStatus::Executed);
    EXPECT_EQ(node.consent_ledger().find(challenge_id)->decided_by,
              "headed app");
}

TEST_F(ConsentFixture, DenialMeansTheActionNeverRuns) {
    const std::string requester =
        make_key("caller", {"add_nodes", "deny_consent"});
    auto refusal =
        call("add_nodes", {{"target", "azure"}, {"count", 1}}, requester);
    const std::string challenge_id = refusal.error.details.value("id", "");

    // The requester may withdraw its own challenge (denial is fail-safe).
    auto denial = call("deny_consent",
                       {{"challenge_id", challenge_id},
                        {"reason", "changed my mind"}},
                       requester);
    ASSERT_TRUE(denial.ok) << denial.error.message;
    EXPECT_EQ(node.consent_ledger().find(challenge_id)->status,
              ConsentStatus::Denied);

    // Denied is terminal: even a legitimate approver cannot revive it.
    const std::string approver = make_key("approver", {"approve_consent"});
    auto late_approval =
        call("approve_consent", {{"challenge_id", challenge_id}}, approver);
    EXPECT_FALSE(late_approval.ok);
    EXPECT_EQ(fake_azure.provisions, 0);
}

// The input-aware gate: the `local` target brings up no machines and touches
// no cloud, so it executes directly — the same plain-click parity the GUI has.
TEST_F(ConsentFixture, LocalTargetExecutesWithoutAChallenge) {
    auto result = call("add_nodes", {{"target", "local"}, {"count", 1}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.value("status", ""), "provisioning");
    wait_for_background();
    EXPECT_EQ(fake_azure.provisions, 1);
    EXPECT_TRUE(node.consent_ledger().all().empty());
}

// The challenge for a cloud inspect names the provider's exact config paths
// and read-only commands — the Cloud Inspector grammar, verbatim.
TEST_F(ConsentFixture, CloudInspectChallengeNamesExactlyWhatWouldBeReadAndRun) {
    auto refusal = call("cloud_inspect", {{"provider", "azure"}});
    ASSERT_FALSE(refusal.ok);
    EXPECT_EQ(refusal.error.code, "consent_required");
    const nlohmann::json& action = refusal.error.details.at("action");
    const auto* azure =
        zetta::cloud::CloudProviderCatalog::builtin().find("azure");
    ASSERT_TRUE(azure);
    ASSERT_EQ(action.at("reads").size(), azure->config_paths.size());
    for (std::size_t i = 0; i < azure->config_paths.size(); ++i) {
        EXPECT_EQ(action.at("reads")[i], azure->config_paths[i]);
    }
    ASSERT_EQ(action.at("runs").size(), azure->inspect_commands.size());
    for (std::size_t i = 0; i < azure->inspect_commands.size(); ++i) {
        EXPECT_EQ(action.at("runs")[i], azure->inspect_commands[i].display);
    }
}

// detect_provisioners is consent-gated too (it queries the local cloud CLIs).
TEST_F(ConsentFixture, DetectProvisionersIsConsentGated) {
    auto refusal = call("detect_provisioners", nlohmann::json::object());
    ASSERT_FALSE(refusal.ok);
    EXPECT_EQ(refusal.error.code, "consent_required");
    EXPECT_FALSE(refusal.error.details.at("action").at("runs").empty());
}

// An add on a cloud target with no wired provisioner fails with the engine's
// reason AFTER approval — approval never conjures a backend.
TEST_F(ConsentFixture, ApprovedActionWithoutAWiredProvisionerFailsClearly) {
    auto refusal = call("add_nodes", {{"target", "gcp"}, {"count", 1}});
    ASSERT_FALSE(refusal.ok);
    const std::string challenge_id = refusal.error.details.value("id", "");

    ApiContext context = context_for({});
    auto decided = registry.decide_consent(
        challenge_id, /*approve=*/true, {}, context,
        Registry::ConsentSurface::HeadedApp);
    EXPECT_FALSE(decided.ok);
    EXPECT_EQ(decided.error.code, "engine_error");
    EXPECT_EQ(node.consent_ledger().find(challenge_id)->status,
              ConsentStatus::Failed);
}

// system.consent_requests surfaces the ledger (RULE-7) — asserted through the
// sql capability like every other read view.
TEST_F(ConsentFixture, ConsentRequestsAreReadableAsASystemView) {
    call("add_nodes", {{"target", "azure"}, {"count", 1}});
    auto rows = call(
        "sql", {{"statement", "SELECT * FROM system.consent_requests;"}});
    ASSERT_TRUE(rows.ok) << rows.error.message;
    ASSERT_EQ(rows.value.at("rows").size(), 1u);
    EXPECT_EQ(rows.value.at("rows")[0].at("capability"), "add_nodes");
    EXPECT_EQ(rows.value.at("rows")[0].at("status"), "pending");

    // And through the read capability agents use.
    auto listed = call("list_consent_requests", nlohmann::json::object());
    ASSERT_TRUE(listed.ok);
    EXPECT_EQ(listed.value.at("consent_requests").size(), 1u);
}

// --- The gate judges the source, not the tool name (#994, #995) ---------------

// `create_dataset_from_query` was gated per-tool with a fixed sentence that
// called every source Snowflake and every source a cloud. Both are per-call
// facts, and its describer now reads them off the resolved connector.
TEST_F(ConsentFixture, ALoopbackPostgresSourceIsNotACloudAndRunsUngated) {
    ASSERT_TRUE(call("create_connection",
                     {{"name", "local-pg"},
                      {"connector",
                       {{"type", "postgres"},
                        {"host", "127.0.0.1"},
                        {"port", "5433"},
                        {"user", "motive"},
                        {"token", "motive"},
                        {"database", "motive"},
                        {"sslmode", "disable"}}}})
                    .ok);

    auto result =
        call("create_dataset_from_query",
             {{"connection", "local-pg"},
              {"query", "SELECT table_name FROM information_schema.tables"}});
    // It reaches the handler — which fails on the unreachable local Postgres,
    // not on consent. The point is which error: never "consent_required".
    EXPECT_NE(result.error.code, "consent_required") << result.error.message;
    EXPECT_TRUE(node.consent_ledger().pending().empty());
}

TEST_F(ConsentFixture, ARemoteSourceChallengesAndTheSummaryNamesItsRealType) {
    ASSERT_TRUE(call("create_connection",
                     {{"name", "warehouse"},
                      {"connector",
                       {{"type", "postgres"},
                        {"host", "db.example.com"},
                        {"port", "5432"},
                        {"user", "readonly"},
                        {"token", "secret"},
                        {"database", "motive"}}}})
                    .ok);

    auto result = call("create_dataset_from_query",
                       {{"connection", "warehouse"},
                        {"query", "SELECT 1"}});
    ASSERT_FALSE(result.ok);
    ASSERT_EQ(result.error.code, "consent_required");
    const std::string summary =
        result.error.details.at("action").value("summary", "");
    EXPECT_NE(summary.find("postgres db.example.com:5432/motive"),
              std::string::npos)
        << summary;
    EXPECT_EQ(summary.find("Snowflake"), std::string::npos)
        << "the summary must not name a connector the caller never used: "
        << summary;
    // What would run is the caller's own query, named verbatim.
    EXPECT_EQ(result.error.details.at("action").at("runs").size(), 1u);
    EXPECT_EQ(result.error.details.at("action").at("runs")[0], "SELECT 1");
}

// --- An inline credential never rides a challenge out (#74, CTL-3) -----------

// A remote source given inline (no saved connection) is consent-gated, and the
// challenge freezes the input to execute it. That input carries the token, and
// the challenge is returned in the refusal and listed to a DIFFERENT principal
// by list_consent_requests — neither may carry the credential.
TEST_F(ConsentFixture, AnInlineConnectorTokenNeverAppearsInTheChallenge) {
    const std::string token = "pat-inline-s3cr3t-74";
    auto result = call("create_dataset_from_query",
                       {{"connector",
                         {{"type", "snowflake"},
                          {"account", "ab12345.us-east-1"},
                          {"token", token},
                          {"warehouse", "COMPUTE_WH"}}},
                        {"query", "SELECT 1"}});
    ASSERT_FALSE(result.ok);
    ASSERT_EQ(result.error.code, "consent_required") << result.error.message;
    EXPECT_EQ(result.error.details.dump().find(token), std::string::npos)
        << result.error.details.dump();
    EXPECT_EQ(result.error.message.find(token), std::string::npos);
    EXPECT_EQ(result.error.details.at("input").at("connector").at("token"),
              "[redacted]");
    EXPECT_EQ(result.error.details.at("input").at("connector").at("account"),
              "ab12345.us-east-1");

    auto listed = call("list_consent_requests", nlohmann::json::object());
    ASSERT_TRUE(listed.ok);
    ASSERT_EQ(listed.value.at("consent_requests").size(), 1u);
    EXPECT_EQ(listed.value.dump().find(token), std::string::npos)
        << listed.value.dump();

    auto rows = call(
        "sql", {{"statement", "SELECT * FROM system.consent_requests;"}});
    ASSERT_TRUE(rows.ok) << rows.error.message;
    EXPECT_EQ(rows.value.dump().find(token), std::string::npos);

    // The ledger itself still holds the real token: approval executes exactly
    // the frozen request, credential included.
    const auto pending = node.consent_ledger().pending();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0]->input.at("connector").at("token"), token);
}

// --- Consent enforcement is OFF by default (#995) -----------------------------

// The gate points a refused caller at approval surfaces that a one-person node
// cannot reach, so it blocks the action instead of gating it. Default OFF: the
// call executes, and the bypass is recorded rather than silent.
struct ConsentDisabledFixture : ConsentFixture {
    zetta::test::TempDir open_dir;
    // The default NodeConfig — no enforce_consent argument at all, so this
    // fails if the default ever flips back without a deliberate decision.
    zetta::Node open_node{
        zetta::NodeConfig{open_dir.path().string(), /*preflight=*/false}};

    void SetUp() override {
        ConsentFixture::SetUp();
        ASSERT_TRUE(open_node.start());
    }
    void TearDown() override {
        wait_for_background();
        open_node.stop();
        ConsentFixture::TearDown();
    }

    ApiResult call_open(const std::string& name, nlohmann::json input) {
        ApiContext context{open_node,
                           nullptr,
                           {},
                           trace,
                           &open_node.span_recorder(),
                           &background,
                           [this](const std::string& target)
                               -> zetta::cloud::Provisioner* {
                               if (target == "azure" || target == "local") {
                                   return &fake_azure;
                               }
                               return nullptr;
                           }};
        return registry.call(name, input, context);
    }

    bool open_audit_contains(zetta::governance::AuditAction action) {
        for (const auto& event : open_node.audit_events()) {
            if (event->value().action == action) return true;
        }
        return false;
    }
};

TEST_F(ConsentDisabledFixture, DefaultConfigLeavesConsentEnforcementOff) {
    EXPECT_FALSE(zetta::NodeConfig{}.enforce_consent);
}

TEST_F(ConsentDisabledFixture, AGatedCallExecutesAndTheBypassIsAudited) {
    auto result = call_open("add_nodes", {{"target", "azure"}, {"count", 2}});
    ASSERT_TRUE(result.ok) << result.error.code << ": " << result.error.message;

    // It really ran — the provisioner was reached, not merely un-refused. The
    // cloud half rides a background worker either way, so pump it first.
    wait_for_background();
    EXPECT_EQ(fake_azure.provisions, 1);
    // No challenge was minted: there is nothing for anyone to approve.
    EXPECT_TRUE(open_node.consent_ledger().pending().empty());
    // Ungated is not unrecorded.
    EXPECT_TRUE(
        open_audit_contains(zetta::governance::AuditAction::ConsentBypassed));
    EXPECT_FALSE(
        open_audit_contains(zetta::governance::AuditAction::ConsentRequested));

    // The bypass row names what ran, so the audit trail still answers "what
    // touched a cloud on this node?" without the challenge.
    std::string detail;
    for (const auto& event : open_node.audit_events()) {
        if (event->value().action ==
            zetta::governance::AuditAction::ConsentBypassed) {
            detail = event->value().detail;
        }
    }
    EXPECT_NE(detail.find("2 billable nodes"), std::string::npos) << detail;
}

// The describer still decides: `add_nodes {target: "local"}` touches no cloud,
// so with enforcement OFF it executes for the same reason it always did — and
// mints no bypass row, because nothing was bypassed.
TEST_F(ConsentDisabledFixture, ALocalTargetIsNotRecordedAsABypass) {
    auto result = call_open("add_nodes", {{"target", "local"}, {"count", 1}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(
        open_audit_contains(zetta::governance::AuditAction::ConsentBypassed));
}

}  // namespace
