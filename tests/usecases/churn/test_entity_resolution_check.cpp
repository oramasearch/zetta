// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/usecases/churn/entity_resolution_check.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/validation.hpp"

using zetta::pipeline::CheckResult;
using zetta::pipeline::CheckStatus;
using zetta::pipeline::CorrectnessGate;
using zetta::usecases::churn::EntityResolutionCheck;
using zetta::pipeline::ValidationReport;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;

namespace {

// One resolved record as the eval reads it: the raw mention, the ground-truth
// entity (the division it truly belongs to), and the account the step assigned.
struct Record {
    std::string mention;
    std::string entity;
    std::string resolved;
};

// Build the `entity_resolution` attribute payload from a list of records.
nlohmann::json resolution_attribute(const std::vector<Record>& records) {
    nlohmann::json items = nlohmann::json::array();
    for (const Record& record : records) {
        items.push_back({{"mention", record.mention},
                         {"entity", record.entity},
                         {"resolved", record.resolved}});
    }
    return nlohmann::json{{"entity_resolution", {{"records", items}}}};
}

// A graph with a single resolve step carrying the given records.
ExecutionGraph graph_with_resolution(const std::vector<Record>& records) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts", resolution_attribute(records));
    return graph;
}

// The Contoso fixture: the make-or-break case. Five sibling subsidiaries, each its
// own CRM account; a *division-correct* resolver keeps them apart.
std::vector<Record> contoso_resolved_correctly() {
    return {
        {"Contoso Freight",    "contoso_freight",   "acct_contoso_freight"},
        {"Contoso Ground",     "contoso_ground",    "acct_contoso_ground"},
        {"Contoso Express",    "contoso_express",   "acct_contoso_express"},
        {"Contoso Office",     "contoso_office",    "acct_contoso_office"},
        {"Contoso Logistics",  "contoso_logistics", "acct_contoso_logistics"},
    };
}

const CheckResult evaluate(const ExecutionGraph& graph) {
    return EntityResolutionCheck{}.evaluate(graph);
}

}  // namespace

// ---- the eval's identity ----------------------------------------------------

TEST(EntityResolutionCheck, ReportsItsStableName) {
    EXPECT_EQ(EntityResolutionCheck{}.name(), "entity_resolution");
}

// ---- division-correct resolution passes -------------------------------------

TEST(EntityResolutionCheck, DivisionCorrectFixturePasses) {
    const CheckResult result = evaluate(graph_with_resolution(contoso_resolved_correctly()));
    EXPECT_EQ(result.name, "entity_resolution");
    EXPECT_EQ(result.status, CheckStatus::Pass);
    EXPECT_FALSE(result.reason.empty());
}

TEST(EntityResolutionCheck, NoResolutionStepPassesVacuously) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "load_crm");  // no entity_resolution attribute
    graph.add_node(StepKind::Serve, "expose");
    const CheckResult result = evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::NotApplicable);
}

TEST(EntityResolutionCheck, EmptyRecordSetHasNothingToConflate) {
    const CheckResult result = evaluate(graph_with_resolution({}));
    EXPECT_EQ(result.status, CheckStatus::Pass);
}

// ---- conflation is the make-or-break failure (RULE-1) -----------------------

TEST(EntityResolutionCheck, ConflatingTwoSiblingsFails) {
    // "Contoso Freight" and "Contoso Ground" are distinct divisions (distinct CRM
    // accounts), but the step merged both onto one account — the canonical TC-4
    // failure. The gate must fail the graph.
    const CheckResult result = evaluate(graph_with_resolution({
        {"Contoso Freight", "contoso_freight", "acct_contoso"},
        {"Contoso Ground",  "contoso_ground",  "acct_contoso"},
    }));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    // The reason names the offending account and the entities it merged.
    EXPECT_NE(result.reason.find("acct_contoso"), std::string::npos);
    EXPECT_NE(result.reason.find("contoso_freight"), std::string::npos);
    EXPECT_NE(result.reason.find("contoso_ground"), std::string::npos);
}

TEST(EntityResolutionCheck, OneConflationAmongCorrectSiblingsStillFails) {
    // Four siblings resolve correctly, but Freight gets folded into Ground's
    // account. A single conflation is enough to fail the whole step.
    std::vector<Record> records = contoso_resolved_correctly();
    records[0].resolved = "acct_contoso_ground";  // Freight → Ground's account
    const CheckResult result = evaluate(graph_with_resolution(records));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("acct_contoso_ground"), std::string::npos);
}

// ---- fragmentation is the softer, symmetric error (warn, not block) ---------

TEST(EntityResolutionCheck, FragmentationWarnsButDoesNotFail) {
    // One true division split across two accounts (under-merge). Less dangerous
    // than conflation, so it warns rather than blocking deploy.
    const CheckResult result = evaluate(graph_with_resolution({
        {"Contoso Freight",   "contoso_freight", "acct_contoso_freight"},
        {"Contoso Freight, Inc.", "contoso_freight", "acct_contoso_freight_dup"},
    }));
    EXPECT_EQ(result.status, CheckStatus::Warn);
    EXPECT_NE(result.reason.find("contoso_freight"), std::string::npos);
}

TEST(EntityResolutionCheck, ConflationOutranksFragmentation) {
    // A step with both errors fails — conflation (the blocking error) wins over
    // fragmentation (the warning) in the worst-status aggregation.
    const CheckResult result = evaluate(graph_with_resolution({
        {"Contoso Freight", "contoso_freight", "acct_contoso"},   // conflated…
        {"Contoso Ground",  "contoso_ground",  "acct_contoso"},   // …with Ground
        {"Contoso Express", "contoso_express", "acct_express_a"},  // fragmented…
        {"Contoso Express", "contoso_express", "acct_express_b"},  // …across two
    }));
    EXPECT_EQ(result.status, CheckStatus::Fail);
}

// ---- the schema boundary: a malformed payload fails fast --------------------

TEST(EntityResolutionCheck, MalformedPayloadFailsAtTheBoundary) {
    // `resolved` is required and non-empty; an empty string violates the schema.
    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts",
                   nlohmann::json{{"entity_resolution",
                                   {{"records", {{{"entity", "contoso_freight"},
                                                  {"resolved", ""}}}}}}});
    const CheckResult result = evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("malformed"), std::string::npos);
}

TEST(EntityResolutionCheck, MissingRequiredFieldFailsAtTheBoundary) {
    // A record missing `entity` is rejected, not silently skipped.
    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts",
                   nlohmann::json{{"entity_resolution",
                                   {{"records", {{{"resolved", "acct_contoso"}}}}}}});
    const CheckResult result = evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
}

// ---- aggregation across several resolve steps -------------------------------

TEST(EntityResolutionCheck, AggregatesWorstStatusAcrossSteps) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_ok",
                   resolution_attribute(contoso_resolved_correctly()));
    graph.add_node(StepKind::Clean, "resolve_bad",
                   resolution_attribute({
                       {"Contoso Freight", "contoso_freight", "acct_contoso"},
                       {"Contoso Ground",  "contoso_ground",  "acct_contoso"},
                   }));
    const CheckResult result = evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("resolve_bad"), std::string::npos);
}

// ---- end to end through the default gate (RULE-1) ---------------------------

TEST(EntityResolutionCheck, DefaultGateBlocksDeployOnConflation) {
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    const ValidationReport report = gate.run(graph_with_resolution({
        {"Contoso Freight", "contoso_freight", "acct_contoso"},
        {"Contoso Ground",  "contoso_ground",  "acct_contoso"},
    }));

    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.block_deploy);  // RULE-1: conflation blocks deploy
    // The failing result is the entity-resolution check (non_empty_graph passes).
    bool found_failing_entity_resolution = false;
    for (const CheckResult& check : report.checks) {
        if (check.name == "entity_resolution") {
            EXPECT_EQ(check.status, CheckStatus::Fail);
            found_failing_entity_resolution = true;
        }
    }
    EXPECT_TRUE(found_failing_entity_resolution);
}

TEST(EntityResolutionCheck, DefaultGateAllowsDivisionCorrectResolution) {
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    const ValidationReport report =
        gate.run(graph_with_resolution(contoso_resolved_correctly()));
    EXPECT_TRUE(report.passed());
    EXPECT_FALSE(report.block_deploy);
}
