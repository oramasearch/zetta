#include "zetta/dataset/base_agent_generator.hpp"

#include <string>
#include <utility>
#include <vector>

#include "zetta/agent/curriculum.hpp"      // churn_solution_case, search_solution_case
#include "zetta/agent/solution_graph.hpp"  // attach_reference_action
#include "zetta/dataset/interaction_families.hpp"  // interaction_families, ProjectContext
#include "zetta/dataset/plan_templates.hpp"  // plan_solution_templates, case_from_template

namespace zetta::dataset {

GeneratedDataset BaseAgentDatasetGenerator::generate(const DatasetGenSpec& spec) const {
    GeneratedDataset dataset;
    const int variants = spec.variants_per_subject < 1 ? 1 : spec.variants_per_subject;

    // Collect every plan subject first, so the canary can be a set of WHOLE subjects the
    // model never saw — a real memorization check, not resampled variants of a seen one.
    std::vector<std::pair<std::string, zetta::agent::AdmissionCase>> subjects;
    // The two Components that build + run today (the dogfood core), with their hand-built rich
    // graphs.
    subjects.emplace_back("UC-1 churn", zetta::agent::churn_solution_case());
    subjects.emplace_back("search", zetta::agent::search_solution_case());
    // The rest of the solutions plan as gated templates (#878) — full coverage today
    // (24 use cases · 21 methods · 12 solutions · DSC · autonomy); see the coverage checklist.
    for (const SolutionTemplate& solution_template : plan_solution_templates()) {
        subjects.emplace_back(solution_template.id, case_from_template(solution_template));
    }

    // Each subject → an admission-gated case (reference passes, negative controls fail), with
    // its reference ACTION attached via the solution-graph bridge. Held-out subjects are
    // emitted LAST, per the canary convention on GeneratedDataset.
    const std::size_t reserved =
        held_out_subject_count(subjects.size(), spec.held_out_fraction);
    const auto emit = [&](std::size_t begin, std::size_t end) {
        std::size_t emitted = 0;
        for (std::size_t index = begin; index < end; ++index) {
            for (int variant = 0; variant < variants; ++variant) {
                zetta::agent::AdmissionCase task = subjects[index].second;
                zetta::agent::attach_reference_action(task);
                dataset.cases.push_back(std::move(task));
                ++emitted;
            }
            dataset.subjects.push_back(subjects[index].first);
        }
        return emitted;
    };
    emit(0, subjects.size() - reserved);
    const std::size_t plan_canary = emit(subjects.size() - reserved, subjects.size());

    // The interaction corpus (#918): every OTHER kind of turn a user has — grounded QA about
    // results/data/platform, capability discovery, abstention, and clarification. These carry
    // their own retrieved context (`grounding`) and their own reference action, so they are
    // emitted as-is rather than run through the solution-graph bridge.
    //
    // Contexts are split the same way subjects are: the trailing ones become canary Projects
    // whose numbers the model has never seen. Because the grounded-QA families hold the question
    // fixed and vary the context, a model that memorized answers instead of reading the supplied
    // facts fails the canary outright — which is precisely what we want measured.
    const int interaction_contexts = 30;
    const int phrasings = 4;
    const std::vector<ProjectContext> contexts =
        synthetic_project_contexts(interaction_contexts);
    const std::size_t context_canary =
        held_out_subject_count(contexts.size(), spec.held_out_fraction);
    const std::vector<ProjectContext> train_contexts(
        contexts.begin(), contexts.end() - static_cast<std::ptrdiff_t>(context_canary));
    const std::vector<ProjectContext> canary_contexts(
        contexts.end() - static_cast<std::ptrdiff_t>(context_canary), contexts.end());

    const auto emit_families = [&](const std::vector<ProjectContext>& used) {
        std::size_t emitted = 0;
        if (used.empty()) return emitted;
        for (const InteractionFamily& family : interaction_families(used, phrasings)) {
            for (const zetta::agent::AdmissionCase& interaction : family.cases) {
                dataset.cases.push_back(interaction);
                ++emitted;
            }
            dataset.subjects.push_back(family.name);
        }
        return emitted;
    };
    emit_families(train_contexts);
    // Canary convention: held-out cases are the LAST entries of `cases`.
    dataset.held_out = plan_canary + emit_families(canary_contexts);

    return dataset;
}

}  // namespace zetta::dataset
