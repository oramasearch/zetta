// SPDX-License-Identifier: MIT

#include "zetta/dataset/auto_suggest_generator.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/end_state.hpp"
#include "zetta/dataset/plan_templates.hpp"

namespace zetta::dataset {
namespace {

using zetta::agent::EndState;

EndState suggestions(std::vector<std::pair<std::string, bool>> entries) {
    EndState state;
    for (auto& [key, value] : entries) state.set(std::move(key), value);
    return state;
}

// One data-state scenario: the ask describes the data present; `required` is which use cases to
// suggest (true) vs withhold (false — distractors whose data is ABSENT). case_from_template makes
// it gated: the reference (correct suggestion set) passes, and controls that drop a correct
// suggestion or add a distractor fail — the discrimination signal.
std::vector<SolutionTemplate> auto_suggest_scenarios() {
    return {
        {"data-churn",
         "Subscription accounts, monthly activity, and cancellation events.",
         suggestions({{"suggest.churn", true}, {"suggest.search", false},
                      {"suggest.forecasting", false}})},
        {"data-docs",
         "50,000 support articles + resolved tickets (free text).",
         suggestions({{"suggest.search", true}, {"suggest.churn", false},
                      {"suggest.forecasting", false}})},
        {"data-timeseries",
         "Daily sales per product/location for the last three years.",
         suggestions({{"suggest.forecasting", true}, {"suggest.search", false},
                      {"suggest.churn", false}})},
        {"data-rich",
         "Customers + orders + activity AND a large document corpus.",
         suggestions({{"suggest.churn", true}, {"suggest.search", true},
                      {"suggest.forecasting", false}})},
        {"data-empty",
         "A single empty table with no rows.",
         suggestions({{"suggest.none", true}, {"suggest.churn", false},
                      {"suggest.search", false}, {"suggest.forecasting", false}})},
    };
}

}  // namespace

GeneratedDataset AutoSuggestDatasetGenerator::generate(const DatasetGenSpec& spec) const {
    GeneratedDataset dataset;
    const std::vector<SolutionTemplate> scenarios = auto_suggest_scenarios();
    // Bound the expansion before any variant is built (#1398).
    const std::optional<int> variants =
        bounded_variants(spec, scenarios.size(), &dataset.error);
    if (!variants) return dataset;

    // Held-out data states are emitted LAST (the canary convention on GeneratedDataset): the
    // canary is a whole data state the model never saw, which is what tests whether it
    // generalizes the present-vs-absent rule rather than memorizing these five.
    const std::size_t reserved =
        held_out_subject_count(scenarios.size(), spec.held_out_fraction);
    const auto emit = [&](std::size_t begin, std::size_t end) {
        std::size_t emitted = 0;
        for (std::size_t index = begin; index < end; ++index) {
            for (int variant = 0; variant < *variants; ++variant) {
                // Reuse the gate builder; auto-suggest cases carry no pipeline action (this
                // model emits suggestions, not a pipeline).
                dataset.cases.push_back(case_from_template(scenarios[index]));
                ++emitted;
            }
            dataset.subjects.push_back(scenarios[index].id);
        }
        return emitted;
    };
    emit(0, scenarios.size() - reserved);
    dataset.held_out = emit(scenarios.size() - reserved, scenarios.size());

    return dataset;
}

}  // namespace zetta::dataset
