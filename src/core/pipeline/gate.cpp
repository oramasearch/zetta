#include "zetta/pipeline/gate.hpp"

#include <string>
#include <map>
#include <utility>

#include "zetta/usecases/churn/entity_resolution_check.hpp"
#include "zetta/pipeline/graph_checks.hpp"

namespace zetta::pipeline {

namespace {

// Adapts a name + callable to the Check interface (see make_check). Stamps the
// registered name onto whatever CheckResult the callable returns, so the
// reported name always matches the registered one.
class FunctionCheck final : public Check {
public:
    FunctionCheck(std::string name,
                  std::function<CheckResult(const ir::ExecutionGraph&)> evaluate)
        : name_(std::move(name)), evaluate_(std::move(evaluate)) {}

    std::string name() const override { return name_; }

    CheckResult evaluate(const ir::ExecutionGraph& graph) const override {
        CheckResult result = evaluate_(graph);
        result.name = name_;
        return result;
    }

private:
    std::string name_;
    std::function<CheckResult(const ir::ExecutionGraph&)> evaluate_;
};

std::map<int, ir::ExecutionGraph> solution_stages(const ir::ExecutionGraph& graph,
                                                std::string& error) {
    std::map<int, ir::ExecutionGraph> stages;
    const auto manifest = graph.find_node("solution_manifest");
    const auto declared = manifest->attributes.find("methodologies");
    if (declared == manifest->attributes.end() || !declared->is_array() || declared->empty()) {
        error = "solution manifest must declare its methodology stages";
        return stages;
    }
    std::map<std::string, int> membership;
    for (const auto& node : graph.nodes()) {
        const auto stage = node->attributes.find("solution_stage");
        int index = 0;
        if (stage != node->attributes.end()) {
            if (!stage->is_number_integer() || stage->get<int>() < 1 ||
                stage->get<int>() > static_cast<int>(declared->size())) {
                error = "solution node has an undeclared stage";
                return {};
            }
            index = stage->get<int>();
        } else if (node->kind == ir::StepKind::Model) {
            error = "solution model must belong to a declared methodology stage";
            return {};
        }
        membership[node->id] = index;
        nlohmann::json attributes = node->attributes;
        if (index > 0 && attributes.is_object()) {
            attributes.erase("methodology");
            attributes.erase("solution_stage");
            attributes.erase("preview");
        }
        stages[index].add_node(node->kind, node->name,
                               std::move(attributes), node->id);
    }
    for (const auto& edge : graph.edges()) {
        if (membership.at(edge.from) == membership.at(edge.to)) {
            stages[membership.at(edge.from)].add_edge(edge.from, edge.to, edge.kind);
        } else if (edge.kind != ir::EdgeKind::Control) {
            error = "solution stages may only cross through control edges; data dependencies must stay inside a checked stage";
            return {};
        }
    }
    for (int index = 1; index <= static_cast<int>(declared->size()); ++index) {
        if (!stages.contains(index)) {
            error = "solution graph is missing a declared methodology stage";
            return {};
        }
    }
    return stages;
}

}  // namespace

std::shared_ptr<Check> make_check(
    std::string name,
    std::function<CheckResult(const ir::ExecutionGraph&)> evaluate) {
    return std::make_shared<FunctionCheck>(std::move(name), std::move(evaluate));
}

CheckResult NonEmptyGraphCheck::evaluate(const ir::ExecutionGraph& graph) const {
    if (graph.empty()) {
        return CheckResult{name(), CheckStatus::Fail,
                           "execution graph has no steps; nothing to deploy"};
    }
    return CheckResult{
        name(), CheckStatus::Pass,
        "graph has " + std::to_string(graph.node_count()) + " step(s)"};
}

CorrectnessGate& CorrectnessGate::add_check(std::shared_ptr<Check> check) {
    if (check) {
        checks_.push_back(std::move(check));
    }
    return *this;
}

ValidationReport CorrectnessGate::run(const ir::ExecutionGraph& graph) const {
    ValidationReport report;
    if (graph.find_node("solution_manifest")) {
        for (const auto& check : checks_) {
            if (!check->evaluates_composed_graph()) {
                continue;
            }
            const CheckResult result = check->evaluate(graph);
            report.add("solution/" + result.name, result.status,
                       result.reason);
        }
        std::string error;
        const auto stages = solution_stages(graph, error);
        if (!error.empty()) {
            report.add("solution_composition", CheckStatus::Fail, error);
        } else {
            for (const auto& [index, stage] : stages) {
                for (const auto& check : checks_) {
                    if (check->evaluates_composed_graph()) {
                        continue;
                    }
                    const CheckResult result = check->evaluate(stage);
                    report.add("stage_" + std::to_string(index) + "/" + result.name,
                               result.status, result.reason);
                }
            }
        }
        report.block_deploy = !report.passed();
        return report;
    }
    for (const auto& check : checks_) {
        const CheckResult result = check->evaluate(graph);
        report.add(result.name, result.status, result.reason);
    }
    // Baseline policy (RULE-1): block deploy exactly when some check failed.
    // Warnings pass and do not block.
    report.block_deploy = !report.passed();
    return report;
}

CorrectnessGate default_gate() {
    CorrectnessGate gate;
    gate.add_check(std::make_shared<NonEmptyGraphCheck>());
    // The concrete #309 semantic checks: schema/type then data-leakage /
    // point-in-time. Both are opt-in per step (a graph that carries none of their
    // annotations passes them), so they are safe to ship in the baseline gate.
    gate.add_check(std::make_shared<SchemaTypeCheck>());
    gate.add_check(std::make_shared<DataLeakageCheck>());
    gate.add_check(std::make_shared<SnapshotLineageCheck>());
    gate.add_check(std::make_shared<S1PointInTimeCheck>());
    gate.add_check(std::make_shared<S1LabelWindowCheck>());
    gate.add_check(std::make_shared<S1TemporalValidationCheck>());
    gate.add_check(std::make_shared<S1GroupSplitCheck>());
    gate.add_check(std::make_shared<S1CensoringCheck>());
    gate.add_check(std::make_shared<S1CalibrationCheck>());
    gate.add_check(std::make_shared<S1EntityResolutionCheck>());
    gate.add_check(std::make_shared<S1LeakageReviewCheck>());
    gate.add_check(std::make_shared<S1LevelCompositionCheck>());
    gate.add_check(std::make_shared<S2PointInTimeConversionCheck>());
    gate.add_check(std::make_shared<S2PostQualificationLeakageCheck>());
    gate.add_check(std::make_shared<S2CapacityRoutingCheck>());
    gate.add_check(std::make_shared<S3ShortTenureCensoringCheck>());
    gate.add_check(std::make_shared<S3ValueHorizonCheck>());
    gate.add_check(std::make_shared<S3EntityAccountingCheck>());
    gate.add_check(std::make_shared<S4RegulatoryContactCheck>());
    gate.add_check(std::make_shared<S4VulnerableCustomerCheck>());
    gate.add_check(std::make_shared<S4PromiseToPayPointInTimeCheck>());
    gate.add_check(std::make_shared<S12NoAutomatedDenialCheck>());
    gate.add_check(std::make_shared<S12FairnessReviewCheck>());
    gate.add_check(std::make_shared<S12DocumentConfidenceCheck>());
    gate.add_check(std::make_shared<S12LevelCompositionCheck>());
    gate.add_check(std::make_shared<TemporalValidationCheck>());
    gate.add_check(std::make_shared<EntityResolutionGraphCheck>());
    gate.add_check(std::make_shared<SegmentationCheck>());
    gate.add_check(std::make_shared<ForecastGraphCheck>());
    gate.add_check(std::make_shared<SurvivalCensoringCheck>());
    gate.add_check(std::make_shared<CausalUpliftCheck>());
    gate.add_check(std::make_shared<AnomalyGraphCheck>());
    gate.add_check(std::make_shared<NextBestActionGraphCheck>());
    gate.add_check(std::make_shared<RankCorrectnessCheck>());
    gate.add_check(std::make_shared<LanguageGroundingCheck>());
    gate.add_check(std::make_shared<LanguageTrainingAccessCheck>());
    gate.add_check(std::make_shared<DocumentReviewCheck>());
    gate.add_check(std::make_shared<GraphEdgeTimeCheck>());
    gate.add_check(std::make_shared<VisionSourceSplitCheck>());
    gate.add_check(std::make_shared<SpeechConsentCheck>());
    gate.add_check(std::make_shared<SpeechPiiRedactionCheck>());
    gate.add_check(std::make_shared<SpeechLocalAudioCheck>());
    // M-15 requires a feasible positive exploration floor and effective caps
    // before any decision can be made.
    gate.add_check(std::make_shared<BanditPolicyCheck>());
    gate.add_check(std::make_shared<OptimizationContractCheck>());
    gate.add_check(std::make_shared<UncertaintyValidationCheck>());
    gate.add_check(std::make_shared<EnsembleOutOfFoldCheck>());
    gate.add_check(std::make_shared<LabelEfficiencyGoldIsolationCheck>());
    gate.add_check(std::make_shared<PrivacyBudgetCheck>());
    gate.add_check(std::make_shared<StreamingCorrectnessCheck>());
    // The entity-resolution correctness eval (#312 / TC-4). A no-op (vacuous
    // pass) on graphs without an entity-resolution step, so it is safe in the
    // baseline gate; it blocks deploy only when a step conflates accounts.
    gate.add_check(std::make_shared<zetta::usecases::churn::EntityResolutionCheck>());
    return gate;
}

} // namespace zetta::pipeline
