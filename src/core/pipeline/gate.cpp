#include "zetta/pipeline/gate.hpp"

#include <string>
#include <utility>

#include "zetta/usecases/churn/entity_resolution_check.hpp"
#include "zetta/pipeline/graph_checks.hpp"

namespace zetta::pipeline {

namespace {

// Adapts a name + callable to the Check seam (see make_check). Stamps the
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
    // The entity-resolution correctness eval (#312 / TC-4). A no-op (vacuous
    // pass) on graphs without an entity-resolution step, so it is safe in the
    // baseline gate; it blocks deploy only when a step conflates accounts.
    gate.add_check(std::make_shared<zetta::usecases::churn::EntityResolutionCheck>());
    // M-15 requires a feasible positive exploration floor and effective caps
    // before any decision can be made.
    gate.add_check(std::make_shared<BanditPolicyCheck>());
    return gate;
}

}  // namespace zetta::pipeline
