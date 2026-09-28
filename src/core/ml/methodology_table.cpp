#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "zetta/ir/step_kind.hpp"

namespace zetta::ml {

Methodology make_m1_methodology();
Methodology make_m9_methodology();

namespace {

constexpr std::string_view kIssueBase =
    "https://github.com/oramasearch/zetta/issues/";

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

MethodologyInput input(std::string role, bool required,
                       std::string description) {
    return MethodologyInput{std::move(role), required, std::move(description)};
}

Methodology
planned(std::string id, std::string name, std::string family,
        std::vector<std::string> model_kinds,
        std::vector<std::string> solutions, std::vector<std::string> keywords,
        std::vector<MethodologyInput> inputs, std::vector<std::string> outputs,
        std::vector<std::string> metrics, int issue, bool partial = false) {
    Methodology methodology;
    methodology.id = std::move(id);
    methodology.name = std::move(name);
    methodology.family = std::move(family);
    methodology.model_kinds = std::move(model_kinds);
    methodology.solutions = std::move(solutions);
    methodology.keywords = std::move(keywords);
    methodology.inputs = std::move(inputs);
    methodology.outputs = std::move(outputs);
    methodology.metrics = std::move(metrics);
    methodology.issue = std::string(kIssueBase) + std::to_string(issue);
    methodology.partial = partial;
    return methodology;
}

bool contains_value(const std::vector<std::string>& values,
                    const std::string& wanted) {
    return std::any_of(
        values.begin(), values.end(),
        [&](const std::string& value) { return lower(value) == wanted; });
}

} // namespace

const std::vector<Methodology>& methodologies() {
    static const std::vector<Methodology> table = {
        make_m1_methodology(),
        planned("M-2", "Entity resolution", "Identity & structure",
                {"entity_resolution", "entity-resolution"}, {},
                {"resolve identity", "deduplicate", "match records"},
                {input("entity", true, "records to resolve"),
                 input("group", false, "source system or entity type")},
                {"entity map", "match evidence"},
                {"pair precision", "pair recall", "blocking recall"}, 1041),
        planned("M-3", "Segmentation / clustering", "Identity & structure",
                {"segmentation", "clustering", "cluster"}, {"S-9"},
                {"segment", "cluster", "group", "persona", "cohort"},
                {input("entity", true, "entities to segment"),
                 input("feature", true, "attributes used for grouping")},
                {"segment assignments", "segment profiles"},
                {"silhouette", "stability", "cluster size"}, 1042),
        planned(
            "M-4", "Time-series forecasting", "Time & vigilance",
            {"forecast", "forecasting", "time_series"}, {"S-5", "S-6"},
            {"forecast", "demand", "predict next week", "predict next month"},
            {input("time", true, "ordered observation time"),
             input("label", true, "value to forecast"),
             input("group", false, "series identifier")},
            {"forecasts", "prediction intervals"},
            {"MAE", "RMSE", "WAPE", "forecast value added"}, 1043),
        planned("M-5", "Survival / time-to-event", "The tabular decision core",
                {"survival", "time_to_event"}, {"S-8"},
                {"time to", "when will", "survival", "remaining useful life"},
                {input("entity", true, "entity whose event time is modeled"),
                 input("time", true, "duration or event time"),
                 input("label", true, "event and censor indicator")},
                {"risk curves", "expected time to event"},
                {"concordance", "time-dependent AUC", "IPCW Brier"}, 1044),
        planned("M-6", "Causal inference & uplift", "Action & decision",
                {"causal", "uplift"}, {},
                {"uplift", "treatment", "who to target", "incremental effect"},
                {input("entity", true, "unit receiving treatment"),
                 input("treatment", true, "treatment assignment"),
                 input("label", true, "observed outcome")},
                {"treatment effects", "targeting policy"},
                {"uplift", "Qini", "policy value"}, 1045),
        planned("M-7", "Anomaly detection", "Time & vigilance",
                {"anomaly", "anomaly_detection", "outlier"}, {"S-7"},
                {"anomaly", "outlier", "unusual", "spike"},
                {input("time", false, "observation time"),
                 input("feature", true, "signals to monitor")},
                {"anomaly scores", "alerts"},
                {"precision", "recall", "false alerts"}, 1046),
        planned("M-8", "Recommendation / next-best-action", "Action & decision",
                {"recommendation", "recommender", "next_best_action"}, {"S-11"},
                {"recommend", "next best action", "next-best-action"},
                {input("entity", true, "recipient of the action"),
                 input("treatment", true, "candidate actions or items"),
                 input("label", false, "interaction or reward")},
                {"ranked actions", "recommendations"},
                {"NDCG", "recall at k", "policy value"}, 1047),
        make_m9_methodology(),
        planned("M-10", "NLP / embeddings / RAG / fine-tuning",
                "Language, documents & perception",
                {"nlp", "embedding", "embeddings", "rag", "fine_tuning",
                 "retrieval"},
                {}, {"text", "semantic search", "retrieve", "rag", "language"},
                {input("text", true, "documents or utterances"),
                 input("label", false, "task labels or golden answers")},
                {"embeddings", "search results", "generated text"},
                {"recall at k", "MRR", "F1", "groundedness"}, 1049, true),
        planned("M-11", "Document intelligence",
                "Language, documents & perception",
                {"document_intelligence", "ocr", "document"}, {},
                {"extract document", "ocr", "invoice", "form"},
                {input("text", true, "document image or text"),
                 input("label", false, "field annotations")},
                {"structured fields", "evidence links"},
                {"field accuracy", "F1", "straight-through rate"}, 1050),
        planned("M-12", "Graph ML", "Identity & structure",
                {"graph_ml", "graph"}, {},
                {"graph", "network", "link prediction"},
                {input("entity", true, "graph nodes"),
                 input("group", true, "relationships or edges"),
                 input("time", false, "edge observation time")},
                {"node scores", "links", "graph embeddings"},
                {"average precision", "AUC", "lift"}, 1051),
        planned("M-13", "Computer vision", "Language, documents & perception",
                {"computer_vision", "vision"}, {},
                {"image", "vision", "detect defect", "segment image"},
                {input("image", true, "images or video frames"),
                 input("label", false, "class, box, or mask annotations")},
                {"classifications", "detections", "masks"},
                {"mAP", "IoU", "escape rate"}, 1052),
        planned("M-14", "Speech & audio analytics",
                "Language, documents & perception", {"speech", "audio"}, {},
                {"speech", "audio", "transcribe", "diarize"},
                {input("audio", true, "audio recordings"),
                 input("label", false, "transcripts or task labels")},
                {"transcripts", "speaker segments", "audio scores"},
                {"word error rate", "F1"}, 1053),
        planned("M-15", "Bandits & sequential decisioning", "Action & decision",
                {"bandit", "bandits", "sequential_decisioning"}, {},
                {"bandit", "explore", "sequential decision"},
                {input("entity", true, "decision context"),
                 input("treatment", true, "available actions"),
                 input("label", true, "observed reward")},
                {"action policy", "propensities"},
                {"cumulative reward", "regret", "policy value"}, 1054),
        planned("M-16", "Optimization / operations research",
                "Action & decision", {"optimization", "operations_research"},
                {}, {"optimize", "schedule", "allocate", "constraints"},
                {input("feature", true, "objective inputs"),
                 input("group", true, "constraints and resources")},
                {"plan", "allocation", "constraint trace"},
                {"objective value", "constraint violations"}, 1055),
        planned(
            "M-17",
            "Bayesian / probabilistic modeling & uncertainty quantification",
            "The tabular decision core",
            {"bayesian", "probabilistic", "uncertainty"}, {},
            {"bayesian", "uncertainty", "credible interval", "probabilistic"},
            {input("feature", true, "observations"),
             input("group", false, "hierarchical group")},
            {"posterior", "prediction intervals"},
            {"coverage", "CRPS", "calibration"}, 1056),
        planned("M-18", "Ensembling & stacking", "The tabular decision core",
                {"ensemble", "ensembling", "stacking"}, {},
                {"ensemble", "stack", "blend models"},
                {input("feature", true, "base-model predictions"),
                 input("label", true, "evaluation outcome")},
                {"ensemble model", "blended predictions"},
                {"delta over best model", "stability"}, 1057),
        planned("M-19", "Label efficiency", "The data supply chain",
                {"label_efficiency", "active_learning", "weak_supervision"}, {},
                {"active learning", "weak supervision", "label efficiency"},
                {input("feature", true, "unlabelled examples"),
                 input("label", false, "gold labels")},
                {"labels", "review queue", "label provenance"},
                {"agreement", "cost per label", "downstream lift"}, 1058),
        planned("M-20", "Privacy-preserving learning & synthetic data",
                "The data supply chain",
                {"privacy_preserving", "federated", "synthetic_data"}, {},
                {"federated", "differential privacy", "synthetic data"},
                {input("group", true, "data boundary or participant"),
                 input("feature", true, "protected training data")},
                {"private model", "synthetic dataset", "guarantee report"},
                {"privacy budget", "TSTR", "attack success"}, 1059),
        planned("M-21", "Online / streaming learning", "Time & vigilance",
                {"online_learning", "streaming"}, {},
                {"online learning", "streaming", "real time learning"},
                {input("time", true, "event time"),
                 input("feature", true, "event features"),
                 input("label", false, "delayed outcome")},
                {"online model", "streaming scores"},
                {"prequential loss", "drift", "latency"}, 1060),
    };
    return table;
}

const Methodology*
find_methodology(std::string_view id_or_model_kind_or_solution) {
    const std::string wanted = lower(id_or_model_kind_or_solution);
    for (const Methodology& methodology : methodologies()) {
        if (lower(methodology.id) == wanted ||
            contains_value(methodology.model_kinds, wanted) ||
            contains_value(methodology.solutions, wanted)) {
            return &methodology;
        }
    }
    return nullptr;
}

const Methodology* methodology_for_graph(const ir::ExecutionGraph& graph) {
    for (const Methodology& methodology : methodologies()) {
        if (methodology.owns && methodology.owns(graph)) {
            return &methodology;
        }
    }
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        if (const auto kind = node->attributes.find("model_kind");
            kind != node->attributes.end() && kind->is_string()) {
            return find_methodology(kind->get<std::string>());
        }
        if (const auto algorithm = node->attributes.find("algo");
            algorithm != node->attributes.end() && algorithm->is_string()) {
            return find_methodology(algorithm->get<std::string>());
        }
    }
    return nullptr;
}

} // namespace zetta::ml
