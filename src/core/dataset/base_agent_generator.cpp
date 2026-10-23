#include "zetta/dataset/base_agent_generator.hpp"

#include <optional>
#include <set>
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
    // Bound the expansion before any variant is built (#1398).
    const std::optional<int> variants =
        bounded_variants(spec, subjects.size(), &dataset.error);
    if (!variants) return dataset;

    // Each subject → an admission-gated case (reference passes, negative controls fail), with
    // its reference ACTION attached via the solution-graph bridge. Held-out subjects are
    // emitted LAST, per the canary convention on GeneratedDataset.
    const std::size_t reserved =
        held_out_subject_count(subjects.size(), spec.held_out_fraction);
    const auto emit = [&](std::size_t begin, std::size_t end) {
        std::size_t emitted = 0;
        for (std::size_t index = begin; index < end; ++index) {
            for (int variant = 0; variant < *variants; ++variant) {
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
    // At least one Project per solution in the plan, and then some. At 30 the contexts could not
    // even reach all 40 templates, so a third of the catalogue never appeared as a Project and the
    // corpus taught a narrower world than the product supports. Contexts are the cheapest axis of
    // diversity there is — they are deterministic, and each one carries a different use case, table,
    // schema and set of measures.
    const int interaction_contexts = 50;
    // How many ways each ask is phrased. Raised from 4 because paraphrase is the ONLY diversity
    // axis an ungrounded case has — it carries no context to vary — and the ungrounded form is how
    // the first message of an evaluation actually arrives.
    const int phrasings = 7;
    const std::vector<ProjectContext> contexts =
        synthetic_project_contexts(interaction_contexts);
    const std::size_t context_canary =
        held_out_subject_count(contexts.size(), spec.held_out_fraction);
    const std::vector<ProjectContext> train_contexts(
        contexts.begin(), contexts.end() - static_cast<std::ptrdiff_t>(context_canary));
    const std::vector<ProjectContext> canary_contexts(
        contexts.end() - static_cast<std::ptrdiff_t>(context_canary), contexts.end());

    // Every (ask, context) pair already emitted. Not every family varies with the ProjectContext
    // — capability discovery asks about the registry, not about a Project — so generating those
    // over the canary contexts reproduces the training cases EXACTLY. Left unchecked that put 105
    // duplicates in the canary, where they measure memorization and report it as generalization.
    // The canary is only worth having if the model has genuinely never seen its contents.
    std::set<std::string> already_emitted;
    const auto emit_families = [&](const std::vector<ProjectContext>& used, bool is_canary) {
        std::size_t emitted = 0;
        if (used.empty()) return emitted;
        for (const InteractionFamily& family : interaction_families(used, phrasings)) {
            bool kept_any = false;
            for (const zetta::agent::AdmissionCase& interaction : family.cases) {
                const std::string key =
                    interaction.task.description() + "\n\x1f" + interaction.grounding;
                if (!already_emitted.insert(key).second) continue;  // a duplicate of a seen case
                dataset.cases.push_back(interaction);
                kept_any = true;
                ++emitted;
            }
            // A family that contributed nothing new is not coverage; don't claim it as a subject.
            if (kept_any) dataset.subjects.push_back(family.name);
        }
        (void)is_canary;
        return emitted;
    };
    emit_families(train_contexts, /*is_canary=*/false);
    // Canary convention: held-out cases are the LAST entries of `cases`.
    dataset.held_out = plan_canary + emit_families(canary_contexts, /*is_canary=*/true);

    return dataset;
}

}  // namespace zetta::dataset
