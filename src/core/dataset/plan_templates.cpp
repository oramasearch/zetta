#include "zetta/dataset/plan_templates.hpp"

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::dataset {
namespace {

using zetta::agent::EndState;

// Build an EndState from {key, value} pairs (terse template authoring).
EndState facts(std::vector<std::pair<std::string, nlohmann::json>> entries) {
    EndState state;
    for (auto& [key, value] : entries) state.set(std::move(key), std::move(value));
    return state;
}

// The shared supervised-decisioning core (the plan's Family I / M-1 correctness profile):
// connect → leakage-free point-in-time features → a fit model → temporal-holdout validation →
// calibration → SHAP explanation → serve. Callers set model_kind + add family specifics.
EndState supervised_core(const std::string& model_kind) {
    return facts({{"connect", true},
                  {"clean", true},
                  {"model", true},
                  {"serve", true},
                  {"leakage_free", true},
                  {"point_in_time", true},
                  {"model_kind", model_kind},
                  {"temporal_validation", true},
                  {"calibrated", true},
                  {"explain", "shap"}});
}

EndState with_fact(EndState state, const std::string& key, nlohmann::json value) {
    state.set(key, std::move(value));
    return state;
}

}  // namespace

std::vector<SolutionTemplate> plan_solution_templates() {
    std::vector<SolutionTemplate> templates;

    // --- Supervised decisioning solutions (Band A) — the sellable core (doc 04, S-n) ---
    templates.push_back(
        {"S-2",
         "Score our leads + opportunities so sales works the ones most likely to convert.",
         supervised_core("gbdt")});
    templates.push_back(
        {"S-3", "Predict each customer's lifetime value from their history.",
         with_fact(supervised_core("regression"), "calibrated", false)});  // regression, no calibration
    templates.push_back(
        {"S-4", "Prioritize which overdue accounts to work first for collections.",
         supervised_core("gbdt")});
    templates.push_back(
        {"S-8", "Predict which machines are likely to fail so we can service them first.",
         supervised_core("survival")});
    templates.push_back(
        {"S-10",
         "Flag likely fraud on transactions for review, with reasons, without over-blocking.",
         with_fact(supervised_core("gbdt"), "anomaly", true)});
    templates.push_back(
        {"S-12", "Triage incoming claims by risk + route the risky ones for review.",
         supervised_core("gbdt")});

    // --- Forecasting + optimization (Family III / IV) ---
    templates.push_back(
        {"S-5", "Forecast demand per product/location for the coming weeks.",
         facts({{"connect", true},
                {"clean", true},
                {"model", true},
                {"serve", true},
                {"leakage_free", true},
                {"model_kind", "forecast"},
                {"temporal_validation", true},
                {"backtest", true}})});  // rolling-origin backtest, not a random split
    templates.push_back(
        {"S-6",
         "Turn the demand forecast into replenishment orders under our stock constraints.",
         facts({{"connect", true},
                {"model", true},
                {"serve", true},
                {"act", true},
                {"model_kind", "forecast"},
                {"temporal_validation", true},
                {"optimization", true},
                {"approval_gated", true}})});  // action layer is approval-gated

    // --- Anomaly / driver analysis, next-best-action (Family III / IV) ---
    templates.push_back(
        {"S-7", "Tell us when a KPI moves abnormally and what's driving it.",
         facts({{"connect", true},
                {"clean", true},
                {"model", true},
                {"serve", true},
                {"model_kind", "anomaly"},
                {"explain", "shap"},
                {"drivers", true}})});
    templates.push_back(
        {"S-11",
         "Recommend the next best action per customer, and only act where it changes the "
         "outcome.",
         facts({{"connect", true},
                {"clean", true},
                {"model", true},
                {"serve", true},
                {"act", true},
                {"leakage_free", true},
                {"model_kind", "uplift"},
                {"incrementality", true},
                {"holdout", true},
                {"approval_gated", true}})});  // uplift/causal, held-out lift, approval-gated

    // --- Language / retrieval (Family V / M-10) ---
    templates.push_back(
        {"M-10",
         "Answer questions over our documents with cited passages, grounded in our data.",
         facts({{"connect", true},
                {"index", true},
                {"serve", true},
                {"index_kind", "semantic"},
                {"retrieval", true},
                {"ranked", true},
                {"grounded_in_data", true},
                {"embeddings", true}})});  // RAG: real retrieval + grounding, not an LLM guess

    return templates;
}

zetta::agent::AdmissionCase case_from_template(const SolutionTemplate& solution_template) {
    zetta::agent::Task task{solution_template.id, solution_template.description, EndState{},
                            {"connect", "clean", "model", "index", "serve", "act"},
                            solution_template.required};

    EndState reference = solution_template.required;  // the correct facts pass their own grader

    // Negative controls: break up to 3 distinct required facts so the grader must discriminate.
    std::vector<EndState> negatives;
    int made = 0;
    for (const auto& [key, value] : solution_template.required.facts()) {
        if (made >= 3) break;
        EndState control = solution_template.required;
        if (value.is_boolean()) {
            control.set(key, !value.get<bool>());
        } else if (value.is_string()) {
            control.set(key, value.get<std::string>() + "__wrong");
        } else {
            control.set(key, false);
        }
        negatives.push_back(std::move(control));
        ++made;
    }
    return zetta::agent::AdmissionCase{std::move(task), std::move(reference), std::move(negatives)};
}

}  // namespace zetta::dataset
