#include "zetta/agent/curriculum.hpp"

#include <string>
#include <utility>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/task.hpp"

namespace zetta::agent {
namespace {

// The facts a correct churn solution must produce. This is the improvement over
// Charles's n8n (Gemini summarizing communications into qualitative signatures, then an
// LLM matching accounts against them — no trained model, no feature vector): a
// leakage-free, gradient-boosted, SHAP-explained, calibrated pipeline.
EndState churn_required() {
    EndState required;
    required.set("connect", true);           // pulls the account events + communications
    required.set("leakage_free", true);      // point-in-time features, no target leakage
    required.set("model_kind", "gbdt");      // gradient-boosted trees, not an LLM
    required.set("explain", "shap");         // SHAP risk drivers
    required.set("calibrated", true);        // calibrated risk scores
    required.set("calibration_loop", true);  // score-vs-outcome tracking (batting average)
    return required;
}

}  // namespace

AdmissionCase churn_solution_case() {
    const std::string description =
        "Build a churn-risk pipeline over the account events + communications: connect "
        "the source, engineer leakage-free point-in-time features, train a "
        "gradient-boosted model with SHAP explanations, and serve calibrated risk scores "
        "with a score-vs-outcome calibration loop.";

    EndState initial;
    initial.set("data.churned_events", true);   // labeled churned-account history
    initial.set("data.active_accounts", true);  // the accounts to score

    // Tools in scope: the six execution-graph step kinds the agent mutates
    // (ir::StepKind — connect/clean/model/index/serve/act).
    Task task{"uc1-churn", description, std::move(initial),
              {"connect", "clean", "model", "index", "serve", "act"}, churn_required()};

    // Reference solution: every required fact satisfied.
    EndState reference = churn_required();

    // Negative controls — plausible-wrong solutions, each failing at least one required
    // fact so the grader must discriminate:
    //  1. his current approach: LLM scoring, not a trained gbdt (+ no SHAP);
    //  2. a leaky pipeline: features peek at the outcome;
    //  3. no calibration loop: scores are never checked against realized churn.
    EndState llm_only = churn_required();
    llm_only.set("model_kind", "llm");
    llm_only.set("explain", "none");

    EndState leaky = churn_required();
    leaky.set("leakage_free", false);

    EndState no_calibration = churn_required();
    no_calibration.set("calibration_loop", false);

    return AdmissionCase{std::move(task), std::move(reference),
                         {std::move(llm_only), std::move(leaky), std::move(no_calibration)}};
}

}  // namespace zetta::agent
