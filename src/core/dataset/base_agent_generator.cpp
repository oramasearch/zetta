#include "zetta/dataset/base_agent_generator.hpp"

#include <string>
#include <utility>

#include "zetta/agent/curriculum.hpp"      // churn_solution_case, search_solution_case
#include "zetta/agent/solution_graph.hpp"  // attach_reference_action

namespace zetta::dataset {

GeneratedDataset BaseAgentDatasetGenerator::generate(const DatasetGenSpec& spec) const {
    GeneratedDataset dataset;
    const int variants = spec.variants_per_subject < 1 ? 1 : spec.variants_per_subject;

    // Each plan subject → an admission-gated case (reference passes, negative controls fail),
    // with its reference ACTION attached via the solution-graph bridge.
    const auto add_subject = [&](const std::string& subject,
                                 const zetta::agent::AdmissionCase& base) {
        for (int variant = 0; variant < variants; ++variant) {
            zetta::agent::AdmissionCase task = base;  // variants identical until paraphrase lands
            zetta::agent::attach_reference_action(task);
            dataset.cases.push_back(std::move(task));
        }
        dataset.subjects.push_back(subject);
    };

    // The two Components that build + run today (the dogfood core). Growing toward full-plan
    // coverage (24 use cases · 21 methods · 11 components · 12 solutions · DSC · autonomy) is the
    // next nibbles — see docs/agent/training-coverage-checklist.md.
    add_subject("UC-1 churn", zetta::agent::churn_solution_case());
    add_subject("search", zetta::agent::search_solution_case());

    return dataset;
}

}  // namespace zetta::dataset
