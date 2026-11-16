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

// The six Contoso siblings plus a near-collision (Umbrella Logistics shares the
// "logistics" token with Contoso Logistics) — a candidate directory shaped like the
// fixture's accounts.
std::vector<ResolutionCandidate> contoso_directory() {
    return {
        {"acct_contoso_freight",   "Contoso Freight",      "Contoso",    "contoso_freight"},
        {"acct_contoso_ground",    "Contoso Ground",       "Contoso",    "contoso_ground"},
        {"acct_contoso_express",   "Contoso Express",      "Contoso",    "contoso_express"},
        {"acct_contoso_office",    "Contoso Office",       "Contoso",    "contoso_office"},
        {"acct_contoso_logistics", "Contoso Logistics",    "Contoso",    "contoso_logistics"},
        {"acct_contoso_services",  "Contoso Services",     "Contoso",    "contoso_services"},
        {"acct_umbrella",        "Umbrella Logistics", "Umbrella", "umbrella"},
    };
}

}  // namespace

// ---- normalization ----------------------------------------------------------

TEST(EntityResolver, NormalizationFoldsCaseAndPunctuation) {
    EXPECT_EQ(normalize_entity_name("Contoso Freight"), "contoso freight");
    EXPECT_EQ(normalize_entity_name("contoso  Freight,"), "contoso freight");
    EXPECT_EQ(normalize_entity_name("  Contoso-Freight  "), "contoso freight");
    EXPECT_EQ(normalize_entity_name("Contoso Freight, Inc."), "contoso freight inc");
    EXPECT_EQ(normalize_entity_name(""), "");
    EXPECT_EQ(normalize_entity_name("!!!"), "");
}

// ---- division-correct full-name resolution (the make-or-break) --------------

TEST(EntityResolver, FullNameKeepsSiblingsApart) {
    const EntityResolver resolver(contoso_directory());
    EXPECT_EQ(resolver.resolve("Contoso Freight").account_id, "acct_contoso_freight");
    EXPECT_EQ(resolver.resolve("Contoso Ground").account_id, "acct_contoso_ground");
    EXPECT_EQ(resolver.resolve("Contoso Express").account_id, "acct_contoso_express");
    EXPECT_EQ(resolver.resolve("Contoso Office").account_id, "acct_contoso_office");
    EXPECT_EQ(resolver.resolve("Contoso Logistics").account_id, "acct_contoso_logistics");
    EXPECT_EQ(resolver.resolve("Contoso Services").account_id, "acct_contoso_services");
}

TEST(EntityResolver, ResolvesACaseVariantSpelling) {
    const EntityResolver resolver(contoso_directory());
    // "contoso Freight" (lowercase 'c') must still land on Contoso Freight's account.
    EXPECT_EQ(resolver.resolve("contoso Freight").account_id, "acct_contoso_freight");
}

TEST(EntityResolver, ResolvesTheNearCollisionToTheRightAccount) {
    const EntityResolver resolver(contoso_directory());
    // "Umbrella Logistics" shares the "logistics" token with "Contoso Logistics" but is
    // a different account; a full-name match must not be fooled.
    EXPECT_EQ(resolver.resolve("Umbrella Logistics").account_id, "acct_umbrella");
    EXPECT_EQ(resolver.resolve("Contoso Logistics").account_id, "acct_contoso_logistics");
}

TEST(EntityResolver, LeavesAnUnmatchableMentionUnresolved) {
    const EntityResolver resolver(contoso_directory());
    // A bare brand or a typo is genuinely ambiguous — the conservative resolver
    // declines rather than guess (and risk conflation).
    EXPECT_FALSE(resolver.resolve("Contoso").resolved());
    EXPECT_FALSE(resolver.resolve("Contoso Frieght").resolved());  // typo
}

// ---- the Brand strategy is the conflation trap ------------------------------

TEST(EntityResolver, BrandCollapsesSiblingsToOneBucket) {
    const EntityResolver resolver(contoso_directory(), ResolutionStrategy::Brand);
    // Every Contoso mention collapses onto one brand bucket — the wrong way.
    const std::string bucket = resolver.resolve("Contoso Freight").account_id;
    EXPECT_EQ(bucket, "acct_brand_contoso");
    EXPECT_EQ(resolver.resolve("Contoso Ground").account_id, "acct_brand_contoso");
    EXPECT_EQ(resolver.resolve("Contoso Logistics").account_id, "acct_brand_contoso");
    // A different brand gets its own bucket.
    EXPECT_EQ(resolver.resolve("Umbrella Logistics").account_id, "acct_brand_umbrella");
}

// ---- the annotation builder + gate ------------------------------------------

TEST(EntityResolver, AnnotationFromCorrectResolutionPassesTheGate) {
    const std::vector<LabeledMention> mentions = {
        {"Contoso Freight",   "contoso_freight"},
        {"contoso Freight",   "contoso_freight"},
        {"Contoso Ground",    "contoso_ground"},
        {"Contoso Logistics", "contoso_logistics"},
        {"Umbrella Logistics", "umbrella"},
    };
    const nlohmann::json attribute = entity_resolution_annotation(contoso_directory(), mentions);

    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts", attribute);
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    const ValidationReport report = gate.run(graph);
    EXPECT_TRUE(report.passed());
    EXPECT_FALSE(report.block_deploy);
}

TEST(EntityResolver, AnnotationFromBrandResolutionFailsTheGate) {
    const std::vector<LabeledMention> mentions = {
        {"Contoso Freight", "contoso_freight"},
        {"Contoso Ground",  "contoso_ground"},
    };
    const nlohmann::json attribute =
        entity_resolution_annotation(contoso_directory(), mentions, ResolutionStrategy::Brand);

    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts", attribute);
    const CheckResult result = EntityResolutionCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("acct_brand_contoso"), std::string::npos);
}

TEST(EntityResolver, UnresolvedMentionGetsADistinctSentinel) {
    const std::vector<LabeledMention> mentions = {
        {"Acme Co", "acme"},        // unmatched
        {"Globex Inc", "globex"},   // unmatched, distinct entity
    };
    const nlohmann::json attribute = entity_resolution_annotation(contoso_directory(), mentions);
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

    // The conflated fixture: resolve with the coarse Brand strategy. The six Contoso
    // siblings collapse onto acct_brand_contoso — the canonical TC-4 conflation.
    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts",
                   entity_resolution_attribute(result.dataset, ResolutionStrategy::Brand));
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    const ValidationReport report = gate.run(graph);
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.block_deploy);  // RULE-1: conflation blocks deploy

    const CheckResult check = EntityResolutionCheck{}.evaluate(graph);
    EXPECT_EQ(check.status, CheckStatus::Fail);
    EXPECT_NE(check.reason.find("acct_brand_contoso"), std::string::npos);
}
