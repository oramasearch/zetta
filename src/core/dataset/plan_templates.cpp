// SPDX-License-Identifier: MIT

#include "zetta/dataset/plan_templates.hpp"

#include <map>
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
                  // Without a holdout the only metric is in-sample, measured on the rows the
                  // model was fit on. An in-sample AUC of 0.868 reached a client report as the
                  // headline once; the held-out figure for the same run was 0.726. A supervised
                  // solution that cannot quote an honest number is not production-grade.
                  {"holdout", true},
                  {"explain", "shap"}});
}

// A FORECAST is not churn with different nouns. Backtested over rolling origins, validated over
// time, and explicitly NOT calibrated — there is no probability to calibrate, and fitting a
// calibrator on rows held out by content re-introduces the leak temporal validation prevents
// (TemporalValidationCheck refuses it at the gate).
EndState forecast_core() {
    return facts({{"connect", true},
                  {"clean", true},
                  {"model", true},
                  {"serve", true},
                  {"model_kind", "forecast"},
                  {"leakage_free", true},
                  {"point_in_time", true},
                  {"temporal_validation", true},
                  {"backtest", true},
                  {"holdout", true},
                  {"explain", "shap"}});
}

// An UNSUPERVISED solution has no label to leak, so `leakage_free` and `calibrated` do not apply.
// What it still owes: a threshold someone can govern, and an explanation per flagged row — an
// anomaly nobody can explain is an alert nobody acts on.
EndState unsupervised_core(const std::string& model_kind) {
    return facts({{"connect", true},
                  {"clean", true},
                  {"model", true},
                  {"serve", true},
                  {"model_kind", model_kind},
                  {"point_in_time", true},
                  {"threshold_governed", true},
                  {"explain", "shap"}});
}

// RETRIEVAL has no supervised label at all — nothing to hold out, calibrate or leak. What makes
// it production-grade instead is that every answer is traceable to the passage it came from:
// `grounded_in_data` is the retrieval equivalent of a held-out metric, and without it the system
// is a fluent guesser.
EndState retrieval_core(const std::string& index_kind) {
    return facts({{"connect", true},
                  {"clean", true},
                  {"index", true},
                  {"index_kind", index_kind},
                  {"serve", true},
                  {"model_kind", "retrieval"},
                  {"grounded_in_data", true},
                  {"retrieval", true}});
}

// DECISIONING (bandits, optimization, next-best-action) does not predict — it CHOOSES, and then
// has to prove the choice was better than the alternative. So the profile is not accuracy but
// counterfactual: an explicit policy, a holdout that never receives the treatment, and an
// approval gate, because a system that acts without one is an incident waiting for a name.
EndState decisioning_core(const std::string& model_kind) {
    return facts({{"connect", true},
                  {"clean", true},
                  {"model", true},
                  {"model_kind", model_kind},
                  {"serve", true},
                  {"act", true},
                  {"holdout", true},
                  {"incrementality", true},
                  {"approval_gated", true},
                  {"explain", "shap"}});
}

// PERCEPTION (vision, speech, document intelligence) turns an artifact into structure, and its
// failure mode is silent: a wrong extraction looks exactly like a right one downstream. So the
// profile is not accuracy but VERIFIABILITY — the output is validated against a schema, and every
// extracted value traces back to the region it came from. A defect nobody is told about ships.
EndState perception_core(const std::string& model_kind, const std::string& explanation) {
    return facts({{"connect", true},
                  {"clean", true},
                  {"model", true},
                  {"model_kind", model_kind},
                  {"serve", true},
                  {"extraction", true},
                  {"grounded_in_data", true},
                  {"holdout", true},
                  {"explain", explanation}});
}

// UNCERTAINTY-FIRST methods (Bayesian, ensembling) exist to say how sure they are. A point
// estimate from one of these is the methodology thrown away — so `uncertainty` is required, and
// `calibrated` means the stated confidence is the observed frequency rather than decoration.
EndState uncertainty_core(const std::string& model_kind) {
    return facts({{"connect", true},
                  {"clean", true},
                  {"model", true},
                  {"model_kind", model_kind},
                  {"serve", true},
                  {"leakage_free", true},
                  {"point_in_time", true},
                  {"calibrated", true},
                  {"uncertainty", true},
                  {"holdout", true},
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
         forecast_core()});  // rolling-origin backtest, not a random split
    templates.push_back(
        {"S-6",
         "Turn the demand forecast into replenishment orders under our stock constraints.",
         with_fact(with_fact(with_fact(forecast_core(), "act", true), "optimization", true), "approval_gated", true)});  // action layer is approval-gated

    // --- Anomaly / driver analysis, next-best-action (Family III / IV) ---
    templates.push_back(
        {"S-7", "Tell us when a KPI moves abnormally and what's driving it.",
         with_fact(unsupervised_core("anomaly"), "drivers", true)});
    templates.push_back(
        {"S-11",
         "Recommend the next best action per customer, and only act where it changes the "
         "outcome.",
         with_fact(decisioning_core("uplift"), "leakage_free", true)});  // uplift/causal, held-out lift, approval-gated

    // --- Language / retrieval (Family V / M-10) ---
    templates.push_back(
        {"M-10",
         "Answer questions over our documents with cited passages, grounded in our data.",
         with_fact(with_fact(retrieval_core("semantic"), "ranked", true), "embeddings", true)});  // RAG: real retrieval + grounding, not an LLM guess

    // --- The methods with no coverage yet (doc 03 §4) — each with its distinguishing checks ---
    templates.push_back(
        {"M-11", "Extract the key fields from our invoices/contracts (touchless AP).",
         with_fact(with_fact(perception_core("document_intelligence", "shap"), "index", true), "schema_validated", true)});  // extracted fields have to be findable
    templates.push_back(
        {"M-12", "Score risk across our account/transaction graph using its relationships.",
         with_fact(supervised_core("graph"), "entity_resolution", true)});
    templates.push_back(
        {"M-13", "Classify/inspect our images for defects.",
         with_fact(perception_core("vision", "gradcam"), "act", true)});
    templates.push_back(
        {"M-14", "Transcribe + analyze our support calls.",
         with_fact(perception_core("speech", "shap"), "transcription", true)});
    templates.push_back(
        {"M-15", "Choose offers online + keep learning which works, safely.",
         with_fact(decisioning_core("bandit"), "exploration", true)});
    templates.push_back(
        {"M-16", "Optimize our plan (routing/allocation) under real constraints.",
         with_fact(decisioning_core("optimization"), "constraints", true)});
    templates.push_back(
        {"M-17", "Give predictions WITH calibrated uncertainty on sparse data.",
         uncertainty_core("bayesian")});
    templates.push_back(
        {"M-18", "Squeeze the most accuracy out by combining models, honestly.",
         with_fact(uncertainty_core("ensemble"), "temporal_validation", true)});
    templates.push_back(
        {"M-19", "Get a good model with very few labels (label-efficient).",
         with_fact(with_fact(supervised_core("classifier"), "label_efficient", true), "active_learning", true)});
    templates.push_back(
        {"M-20", "Train on sensitive data without exposing it (privacy-preserving).",
         with_fact(with_fact(supervised_core("classifier"), "privacy_preserving", true), "synthetic_data", true)});
    templates.push_back(
        {"M-21", "Keep the model fresh as data streams in, watching for drift.",
         with_fact(with_fact(with_fact(supervised_core("online"), "streaming", true), "drift_monitoring", true), "act", true)});

    // --- Remaining use cases (doc 02, UC-13…24) mapped to their methodology ---
    templates.push_back(
        {"UC-13", "Decide credit risk on applicants, defensibly (regulated).",
         with_fact(supervised_core("gbdt"), "baseline_glm", true)});  // SR 11-7: GLM baseline
    templates.push_back({"UC-14", "Optimize our prices/discounts within guardrails.",
                         with_fact(decisioning_core("optimization"), "constraints", true)});
    templates.push_back({"UC-15", "Predict which employees are at risk of leaving.",
                         supervised_core("gbdt")});
    templates.push_back(
        {"UC-16", "Triage + route incoming support tickets automatically.",
         with_fact(with_fact(with_fact(supervised_core("classifier"), "act", true), "grounded_in_data", true), "approval_gated", true)});
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
                         forecast_core()});
    templates.push_back({"UC-21", "Detect + explain incidents across our systems (AIOps).",
                         with_fact(with_fact(with_fact(with_fact(unsupervised_core("anomaly"), "act", true), "streaming", true), "drivers", true), "drift_monitoring", true)});
    templates.push_back({"UC-22", "Find what drives scrap/quality/yield in our process.",
                         with_fact(supervised_core("gbdt"), "drivers", true)});
    templates.push_back({"UC-23", "Target our campaigns at who'll actually respond because of them.",
                         with_fact(decisioning_core("uplift"), "leakage_free", true)});
    templates.push_back(
        {"UC-24", "Read our contracts + flag obligation/renewal risk.",
         with_fact(with_fact(perception_core("document_intelligence", "shap"), "index", true), "act", true)});

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

    label_solution_templates(templates);
    return templates;
}

// What each solution's Project is called, and the table it works over. The name must imply the
// pipeline — a reader (and a model) seeing "Demand forecast - SKU x location" should expect a
// forecasting pipeline, and seeing "Document Q&A" should expect an indexed one.
void label_solution_templates(std::vector<SolutionTemplate>& templates) {
    static const std::map<std::string, std::pair<const char*, const char*>> kLabels = {
        {"S-2", {"Lead scoring - inbound", "leads"}},
        {"S-3", {"Lifetime value forecast", "customers"}},
        {"S-4", {"Collections priority", "invoices"}},
        {"S-5", {"Demand forecast - SKU x location", "sales"}},
        {"S-6", {"Replenishment planning", "inventory"}},
        {"S-7", {"KPI anomaly watch", "metrics"}},
        {"S-8", {"Equipment failure risk", "machines"}},
        {"S-10", {"Transaction fraud review", "transactions"}},
        {"S-11", {"Next best action", "customers"}},
        {"S-12", {"Claims triage", "claims"}},
        {"M-10", {"Document Q&A", "documents"}},
        {"M-11", {"Invoice field extraction", "invoices"}},
        {"M-12", {"Account graph risk", "accounts"}},
        {"M-13", {"Visual defect inspection", "images"}},
        {"M-14", {"Support call analysis", "calls"}},
        {"M-15", {"Offer selection - online", "offers"}},
        {"M-16", {"Routing + allocation plan", "routes"}},
        {"M-17", {"Sparse-data prediction", "observations"}},
        {"M-18", {"Ensemble accuracy push", "records"}},
        {"M-19", {"Label-efficient training", "samples"}},
        {"M-20", {"Privacy-preserving training", "records"}},
        {"M-21", {"Streaming model freshness", "events"}},
        {"UC-13", {"Credit risk decisioning", "applications"}},
        {"UC-14", {"Price + discount optimization", "products"}},
        {"UC-15", {"Employee attrition risk", "employees"}},
        {"UC-16", {"Support ticket triage", "tickets"}},
        {"UC-18", {"Supplier risk monitoring", "suppliers"}},
        {"UC-19", {"Claim denial + no-show risk", "encounters"}},
        {"UC-20", {"Cash flow forecast", "ledger"}},
        {"UC-21", {"Incident detection - AIOps", "telemetry"}},
        {"UC-22", {"Yield + scrap drivers", "batches"}},
        {"UC-23", {"Campaign targeting - incremental", "audiences"}},
        {"UC-24", {"Contract obligation risk", "contracts"}},
    };
    for (SolutionTemplate& solution_template : templates) {
        const auto found = kLabels.find(solution_template.id);
        if (found != kLabels.end()) {
            solution_template.project = found->second.first;
            solution_template.table = found->second.second;
        } else {
            // A template with no label still gets a coherent one rather than someone else's.
            solution_template.project = solution_template.id;
            solution_template.table = "records";
        }
    }
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
