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
         // Servicing is the outcome; a maintenance model that raises no work order is shelfware.
         with_fact(with_fact(supervised_core("survival"), "act", true), "approval_gated", true)});
    templates.push_back(
        {"S-10",
         "Flag likely fraud on transactions for review, with reasons, without over-blocking.",
         // The flag IS the action, and "without over-blocking" is what the approval gate is for:
         // a fraud model that auto-blocks is how a business loses good customers.
         with_fact(with_fact(with_fact(supervised_core("gbdt"), "anomaly", true), "act", true),
                   "approval_gated", true)});
    templates.push_back(
        {"S-12", "Triage incoming claims by risk + route the risky ones for review.",
         // "route the risky ones" is an action step; without it this is a scorer, not triage.
         with_fact(with_fact(supervised_core("gbdt"), "act", true), "approval_gated", true)});

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

    // --- The methods with no coverage yet (doc 03 §4) — each with its distinguishing checks ---
    templates.push_back(
        {"M-11", "Extract the key fields from our invoices/contracts (touchless AP).",
         facts({{"connect", true}, {"clean", true}, {"model", true}, {"index", true},
                {"serve", true}, {"model_kind", "document_intelligence"}, {"extraction", true},
                {"grounded_in_data", true},
                {"index_kind", "semantic"}})});  // extracted fields have to be findable
    templates.push_back(
        {"M-12", "Score risk across our account/transaction graph using its relationships.",
         facts({{"connect", true}, {"clean", true}, {"model", true}, {"serve", true},
                {"model_kind", "graph"}, {"entity_resolution", true}, {"leakage_free", true}})});
    templates.push_back(
        {"M-13", "Classify/inspect our images for defects.",
         facts({{"connect", true}, {"model", true}, {"serve", true},
                {"act", true},  // a defect nobody is told about is a defect that ships
                {"model_kind", "vision"}, {"explain", "gradcam"}, {"approval_gated", true}})});
    templates.push_back(
        {"M-14", "Transcribe + analyze our support calls.",
         facts({{"connect", true}, {"model", true}, {"serve", true}, {"model_kind", "speech"}})});
    templates.push_back(
        {"M-15", "Choose offers online + keep learning which works, safely.",
         facts({{"connect", true}, {"model", true}, {"serve", true}, {"act", true},
                {"model_kind", "bandit"}, {"exploration", true}, {"holdout", true},
                {"approval_gated", true}})});
    templates.push_back(
        {"M-16", "Optimize our plan (routing/allocation) under real constraints.",
         facts({{"connect", true}, {"model", true}, {"serve", true}, {"act", true},
                {"model_kind", "optimization"}, {"constraints", true},
                {"approval_gated", true}})});
    templates.push_back(
        {"M-17", "Give predictions WITH calibrated uncertainty on sparse data.",
         facts({{"connect", true}, {"clean", true}, {"model", true}, {"serve", true},
                {"model_kind", "bayesian"}, {"calibrated", true}, {"uncertainty", true}})});
    templates.push_back(
        {"M-18", "Squeeze the most accuracy out by combining models, honestly.",
         facts({{"connect", true}, {"clean", true}, {"model", true}, {"serve", true},
                {"leakage_free", true}, {"model_kind", "ensemble"}, {"temporal_validation", true},
                {"calibrated", true}, {"explain", "shap"}})});
    templates.push_back(
        {"M-19", "Get a good model with very few labels (label-efficient).",
         facts({{"connect", true}, {"clean", true}, {"model", true}, {"serve", true},
                {"label_efficient", true}, {"active_learning", true}, {"calibrated", true}})});
    templates.push_back(
        {"M-20", "Train on sensitive data without exposing it (privacy-preserving).",
         facts({{"connect", true}, {"clean", true}, {"model", true}, {"serve", true},
                {"privacy_preserving", true}, {"synthetic_data", true}})});
    templates.push_back(
        {"M-21", "Keep the model fresh as data streams in, watching for drift.",
         facts({{"connect", true}, {"model", true}, {"serve", true},
                {"act", true},  // drift that raises no alert is drift nobody noticed
                {"model_kind", "online"}, {"streaming", true}, {"drift_monitoring", true},
                {"approval_gated", true}})});

    // --- Remaining use cases (doc 02, UC-13…24) mapped to their methodology ---
    templates.push_back(
        {"UC-13", "Decide credit risk on applicants, defensibly (regulated).",
         with_fact(supervised_core("gbdt"), "baseline_glm", true)});  // SR 11-7: GLM baseline
    templates.push_back({"UC-14", "Optimize our prices/discounts within guardrails.",
                         facts({{"connect", true}, {"model", true}, {"serve", true}, {"act", true},
                                {"model_kind", "optimization"}, {"constraints", true},
                                {"approval_gated", true}})});
    templates.push_back({"UC-15", "Predict which employees are at risk of leaving.",
                         supervised_core("gbdt")});
    templates.push_back(
        {"UC-16", "Triage + route incoming support tickets automatically.",
         facts({{"connect", true}, {"clean", true}, {"model", true}, {"serve", true}, {"act", true},
                {"model_kind", "classifier"}, {"grounded_in_data", true},
                {"approval_gated", true}})});
    templates.push_back(
        {"UC-18", "Monitor our suppliers for emerging risk.",
         // Monitoring that notifies no one is a dashboard nobody opens.
         with_fact(with_fact(with_fact(supervised_core("gbdt"), "monitoring", true), "act", true),
                   "approval_gated", true)});
    templates.push_back(
        {"UC-19", "Predict claim denials + patient no-shows.",
         // The intervention (a reminder, a corrected claim) is where the value is.
         with_fact(with_fact(supervised_core("gbdt"), "act", true), "approval_gated", true)});
    templates.push_back({"UC-20", "Forecast cash flow + working capital.",
                         facts({{"connect", true}, {"model", true}, {"serve", true},
                                {"model_kind", "forecast"}, {"temporal_validation", true},
                                {"backtest", true}})});
    templates.push_back({"UC-21", "Detect + explain incidents across our systems (AIOps).",
                         facts({{"connect", true}, {"model", true}, {"serve", true},
                                {"act", true},  // an undetected-incident alert is the product
                                {"model_kind", "anomaly"}, {"streaming", true},
                                {"drivers", true}, {"drift_monitoring", true}})});
    templates.push_back({"UC-22", "Find what drives scrap/quality/yield in our process.",
                         facts({{"connect", true}, {"clean", true}, {"model", true}, {"serve", true},
                                {"model_kind", "gbdt"}, {"explain", "shap"}, {"drivers", true},
                                {"leakage_free", true}})});
    templates.push_back({"UC-23", "Target our campaigns at who'll actually respond because of them.",
                         facts({{"connect", true}, {"clean", true}, {"model", true}, {"serve", true},
                                {"act", true}, {"leakage_free", true}, {"model_kind", "uplift"},
                                {"incrementality", true}, {"holdout", true},
                                {"approval_gated", true}})});
    templates.push_back(
        {"UC-24", "Read our contracts + flag obligation/renewal risk.",
         facts({{"connect", true}, {"clean", true}, {"model", true}, {"index", true},
                {"serve", true}, {"act", true},  // a renewal you are not told about is one you miss
                {"model_kind", "document_intelligence"}, {"extraction", true},
                {"grounded_in_data", true}, {"index_kind", "semantic"},
                {"approval_gated", true}})});

    // --- The data supply chain (doc 08, DSC-1…8) — the correct data path a pipeline honors ---
    templates.push_back(
        {"DSC", "Connect our systems, resolve entities, and build a leakage-safe history to "
                "learn from — with holdouts kept sacred.",
         facts({{"connect", true}, {"clean", true},
                {"entity_resolution", true},   // DSC-2
                {"point_in_time", true},        // DSC-3 leakage-safe store
                {"leakage_free", true},
                {"labels", true},               // DSC-4 outcomes / reward stream
                {"holdout", true}})});          // DSC-7 evaluation data kept sacred

    // --- Autonomy classes (doc 03 §2.6 / 07 §4.2) — act only under approval ---
    templates.push_back(
        {"AC", "Let it observe + recommend, but require approval before it acts on the world.",
         facts({{"connect", true}, {"model", true}, {"serve", true}, {"act", true},
                {"approval_gated", true}})});  // AC-3 act-under-approval

    // --- Out of scope (ABSTAIN): NOT a data/AI project Orama builds — decline, build nothing ---
    for (const std::string& ask :
         {std::string("Write me a poem about our quarterly numbers."),
          std::string("What's the capital of France?"),
          std::string("Book me a flight to Tokyo next week.")}) {
        templates.push_back({"ABSTAIN", ask, facts({{"abstain", true}, {"in_scope", false}})});
    }

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
