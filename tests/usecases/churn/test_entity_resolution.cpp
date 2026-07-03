#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/usecases/churn/entity_resolution_check.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/entity_resolution.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/validation.hpp"

namespace fs = std::filesystem;
using zetta::pipeline::CheckResult;
using zetta::pipeline::CheckStatus;
using zetta::usecases::churn::ChurnAccount;
using zetta::usecases::churn::ChurnDataset;
using zetta::usecases::churn::ChurnEvent;
using zetta::pipeline::CorrectnessGate;
using zetta::usecases::churn::entity_resolution_annotation;
using zetta::usecases::churn::entity_resolution_attribute;
using zetta::usecases::churn::EntityResolver;
using zetta::usecases::churn::EntityResolutionCheck;
using zetta::usecases::churn::LabeledMention;
using zetta::usecases::churn::load_churn_dataset;
using zetta::usecases::churn::normalize_entity_name;
using zetta::usecases::churn::ResolutionCandidate;
using zetta::usecases::churn::ResolutionStrategy;
using zetta::usecases::churn::ResolvedMention;
using zetta::pipeline::ValidationReport;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;

namespace {

fs::path churn_dir() { return fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn"; }

// The six FedEx siblings plus a near-collision (Umbrella Logistics shares the
// "logistics" token with FedEx Logistics) — a candidate directory shaped like the
// fixture's accounts.
std::vector<ResolutionCandidate> fedex_directory() {
    return {
        {"acct_fedex_freight",   "FedEx Freight",      "FedEx",    "fedex_freight"},
        {"acct_fedex_ground",    "FedEx Ground",       "FedEx",    "fedex_ground"},
        {"acct_fedex_express",   "FedEx Express",      "FedEx",    "fedex_express"},
        {"acct_fedex_office",    "FedEx Office",       "FedEx",    "fedex_office"},
        {"acct_fedex_logistics", "FedEx Logistics",    "FedEx",    "fedex_logistics"},
        {"acct_fedex_services",  "FedEx Services",     "FedEx",    "fedex_services"},
        {"acct_umbrella",        "Umbrella Logistics", "Umbrella", "umbrella"},
    };
}

}  // namespace

// ---- normalization ----------------------------------------------------------

TEST(EntityResolver, NormalizationFoldsCaseAndPunctuation) {
    EXPECT_EQ(normalize_entity_name("FedEx Freight"), "fedex freight");
    EXPECT_EQ(normalize_entity_name("Fedex  Freight,"), "fedex freight");
    EXPECT_EQ(normalize_entity_name("  FedEx-Freight  "), "fedex freight");
    EXPECT_EQ(normalize_entity_name("FedEx Freight, Inc."), "fedex freight inc");
    EXPECT_EQ(normalize_entity_name(""), "");
    EXPECT_EQ(normalize_entity_name("!!!"), "");
}

// ---- division-correct full-name resolution (the make-or-break) --------------

TEST(EntityResolver, FullNameKeepsSiblingsApart) {
    const EntityResolver resolver(fedex_directory());
    EXPECT_EQ(resolver.resolve("FedEx Freight").account_id, "acct_fedex_freight");
    EXPECT_EQ(resolver.resolve("FedEx Ground").account_id, "acct_fedex_ground");
    EXPECT_EQ(resolver.resolve("FedEx Express").account_id, "acct_fedex_express");
    EXPECT_EQ(resolver.resolve("FedEx Office").account_id, "acct_fedex_office");
    EXPECT_EQ(resolver.resolve("FedEx Logistics").account_id, "acct_fedex_logistics");
    EXPECT_EQ(resolver.resolve("FedEx Services").account_id, "acct_fedex_services");
}

TEST(EntityResolver, ResolvesACaseVariantSpelling) {
    const EntityResolver resolver(fedex_directory());
    // "Fedex Freight" (lowercase 'e') must still land on FedEx Freight's account.
    EXPECT_EQ(resolver.resolve("Fedex Freight").account_id, "acct_fedex_freight");
}

TEST(EntityResolver, ResolvesTheNearCollisionToTheRightAccount) {
    const EntityResolver resolver(fedex_directory());
    // "Umbrella Logistics" shares the "logistics" token with "FedEx Logistics" but is
    // a different account; a full-name match must not be fooled.
    EXPECT_EQ(resolver.resolve("Umbrella Logistics").account_id, "acct_umbrella");
    EXPECT_EQ(resolver.resolve("FedEx Logistics").account_id, "acct_fedex_logistics");
}

TEST(EntityResolver, LeavesAnUnmatchableMentionUnresolved) {
    const EntityResolver resolver(fedex_directory());
    // A bare brand or a typo is genuinely ambiguous — the conservative resolver
    // declines rather than guess (and risk conflation).
    EXPECT_FALSE(resolver.resolve("FedEx").resolved());
    EXPECT_FALSE(resolver.resolve("FedEx Frieght").resolved());  // typo
}

// ---- the Brand strategy is the conflation trap ------------------------------

TEST(EntityResolver, BrandCollapsesSiblingsToOneBucket) {
    const EntityResolver resolver(fedex_directory(), ResolutionStrategy::Brand);
    // Every FedEx mention collapses onto one brand bucket — the wrong way.
    const std::string bucket = resolver.resolve("FedEx Freight").account_id;
    EXPECT_EQ(bucket, "acct_brand_fedex");
    EXPECT_EQ(resolver.resolve("FedEx Ground").account_id, "acct_brand_fedex");
    EXPECT_EQ(resolver.resolve("FedEx Logistics").account_id, "acct_brand_fedex");
    // A different brand gets its own bucket.
    EXPECT_EQ(resolver.resolve("Umbrella Logistics").account_id, "acct_brand_umbrella");
}

// ---- the annotation builder + gate ------------------------------------------

TEST(EntityResolver, AnnotationFromCorrectResolutionPassesTheGate) {
    const std::vector<LabeledMention> mentions = {
        {"FedEx Freight",   "fedex_freight"},
        {"Fedex Freight",   "fedex_freight"},
        {"FedEx Ground",    "fedex_ground"},
        {"FedEx Logistics", "fedex_logistics"},
        {"Umbrella Logistics", "umbrella"},
    };
    const nlohmann::json attribute = entity_resolution_annotation(fedex_directory(), mentions);

    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts", attribute);
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    const ValidationReport report = gate.run(graph);
    EXPECT_TRUE(report.passed());
    EXPECT_FALSE(report.block_deploy);
}

TEST(EntityResolver, AnnotationFromBrandResolutionFailsTheGate) {
    const std::vector<LabeledMention> mentions = {
        {"FedEx Freight", "fedex_freight"},
        {"FedEx Ground",  "fedex_ground"},
    };
    const nlohmann::json attribute =
        entity_resolution_annotation(fedex_directory(), mentions, ResolutionStrategy::Brand);

    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts", attribute);
    const CheckResult result = EntityResolutionCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("acct_brand_fedex"), std::string::npos);
}

TEST(EntityResolver, UnresolvedMentionGetsADistinctSentinel) {
    const std::vector<LabeledMention> mentions = {
        {"Acme Co", "acme"},        // unmatched
        {"Globex Inc", "globex"},   // unmatched, distinct entity
    };
    const nlohmann::json attribute = entity_resolution_annotation(fedex_directory(), mentions);
    const auto& records = attribute.at("entity_resolution").at("records");
    ASSERT_EQ(records.size(), 2u);
    // Two distinct unresolved mentions get distinct sentinels, so they do not falsely
    // conflate — but the sentinel is non-empty (schema-valid) and visible.
    const std::string a = records[0].at("resolved").get<std::string>();
    const std::string b = records[1].at("resolved").get<std::string>();
    EXPECT_NE(a, b);
    EXPECT_EQ(a.rfind("unresolved:", 0), 0u);
}

// ---- TC-4 acceptance: real resolution over the committed churn fixture -------

TEST(EntityResolver, ResolvesEveryFixtureMentionToItsTrueAccount) {
    const auto result = load_churn_dataset(churn_dir());
    ASSERT_TRUE(result.ok()) << result.error;
    const ChurnDataset& dataset = result.dataset;

    std::vector<ResolutionCandidate> candidates;
    for (const ChurnAccount& account : dataset.accounts) {
        candidates.push_back(
            {account.account_id, account.name, account.brand, account.entity});
    }
    const EntityResolver resolver(candidates);

    // Every comms mention must resolve to the account it actually belongs to — the
    // resolution is genuine (matched from the mention string), not read off the event.
    std::size_t comms = 0;
    for (const ChurnEvent& event : dataset.events) {
        if (event.mention.empty()) continue;
        ++comms;
        EXPECT_EQ(resolver.resolve(event.mention).account_id, event.account_id)
            << "mention: " << event.mention;
    }
    EXPECT_GT(comms, 0u);
}

TEST(EntityResolver, FixtureFullNameResolutionPassesTheGate) {
    const auto result = load_churn_dataset(churn_dir());
    ASSERT_TRUE(result.ok()) << result.error;

    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts",
                   entity_resolution_attribute(result.dataset));
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    const ValidationReport report = gate.run(graph);
    EXPECT_TRUE(report.passed());
    EXPECT_FALSE(report.block_deploy);
}

TEST(EntityResolver, FixtureBrandResolutionBlocksDeploy) {
    const auto result = load_churn_dataset(churn_dir());
    ASSERT_TRUE(result.ok()) << result.error;

    // The conflated fixture: resolve with the coarse Brand strategy. The six FedEx
    // siblings collapse onto acct_brand_fedex — the canonical TC-4 conflation.
    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts",
                   entity_resolution_attribute(result.dataset, ResolutionStrategy::Brand));
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    const ValidationReport report = gate.run(graph);
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.block_deploy);  // RULE-1: conflation blocks deploy

    const CheckResult check = EntityResolutionCheck{}.evaluate(graph);
    EXPECT_EQ(check.status, CheckStatus::Fail);
    EXPECT_NE(check.reason.find("acct_brand_fedex"), std::string::npos);
}
