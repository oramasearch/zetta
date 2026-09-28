#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/anomaly_methodology.hpp"
#include "zetta/ml/forecast_methodology.hpp"

namespace zetta::ml {

Methodology make_m1_methodology();
Methodology make_m2_methodology();
Methodology make_m3_methodology();
Methodology make_m5_methodology();
Methodology make_m6_methodology();
Methodology make_m8_methodology();
Methodology make_m9_methodology();
Methodology make_m10_methodology();
Methodology make_m11_methodology();
Methodology make_graph_methodology();
Methodology make_m13_methodology();
Methodology make_m14_methodology();
Methodology make_m15_methodology();
Methodology make_m16_methodology();
Methodology make_m17_methodology();
Methodology make_m18_methodology();

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
        make_m2_methodology(),
        make_m3_methodology(),
        make_m4_methodology(),
        make_m5_methodology(),
        make_m6_methodology(),
        make_anomaly_methodology(),
        make_m8_methodology(),
        make_m9_methodology(),
        make_m10_methodology(),
        make_m11_methodology(),
        make_graph_methodology(),
        make_m13_methodology(),
        make_m14_methodology(),
        make_m15_methodology(),
        make_m16_methodology(),
        make_m17_methodology(),
        make_m18_methodology(),
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
            contains_value(methodology.model_kinds, wanted)) {
            return &methodology;
        }
    }
    // A solution may admit multiple methodologies. Prefer one the engine can
    // execute, then retain the portfolio-order fallback for planned entries.
    for (const Methodology& methodology : methodologies()) {
        if (methodology.runnable &&
            contains_value(methodology.solutions, wanted)) {
            return &methodology;
        }
    }
    for (const Methodology& methodology : methodologies()) {
        if (contains_value(methodology.solutions, wanted)) return &methodology;
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
