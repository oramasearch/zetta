// Native dataset generation (#873): the factory lists + creates generators, and the base-agent
// generator produces an admission-GATED corpus (blender-rl: reference passes, controls fail)
// with reference actions attached — the native replacement for the Python generators.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/response_facts.hpp"
#include "zetta/agent/solution_graph.hpp"
#include "zetta/dataset/base_agent_generator.hpp"
#include "zetta/dataset/dataset_generator.hpp"
#include "zetta/dataset/dataset_generator_factory.hpp"

namespace {

TEST(DatasetGenerator, FactoryListsAndCreatesByKind) {
    const zetta::dataset::DatasetGeneratorFactory& factory =
        zetta::dataset::DatasetGeneratorFactory::builtin();
    const std::vector<std::string> kinds = factory.kinds();
    EXPECT_NE(std::find(kinds.begin(), kinds.end(), "base-agent"), kinds.end());

    const std::shared_ptr<const zetta::dataset::DatasetGenerator> generator =
        factory.create("base-agent");
    ASSERT_NE(generator, nullptr);
    EXPECT_EQ(generator->kind(), "base-agent");
    EXPECT_FALSE(generator->title().empty());
    EXPECT_FALSE(generator->description().empty());

    EXPECT_EQ(factory.create("does-not-exist"), nullptr);
}

TEST(DatasetGenerator, BaseAgentProducesAGatedCorpus) {
    const auto generator = zetta::dataset::DatasetGeneratorFactory::builtin().create("base-agent");
    ASSERT_NE(generator, nullptr);

    zetta::dataset::DatasetGenSpec spec;
    spec.variants_per_subject = 2;
    const zetta::dataset::GeneratedDataset dataset = generator->generate(spec);

    // Churn + search + the plan templates (#878), each × 2 variants. Coverage grows, so assert a
    // floor rather than an exact count; every case is gated with a gradeable reference action.
    EXPECT_GE(dataset.subjects.size(), 12u) << "grows toward full-plan coverage";
    // The corpus is no longer one-case-per-subject: the interaction families (#918) expand a
    // cross product of contexts x asks x phrasings under each family's subject entry.
    EXPECT_GE(dataset.cases.size(), dataset.subjects.size() * spec.variants_per_subject);
    for (const zetta::agent::AdmissionCase& admission_case : dataset.cases) {
        ASSERT_TRUE(admission_case.reference_action.has_value()) << "each case carries its action";
        // Grade through the RESPONSE projection, not the pipeline alone: an `answer`, a
        // `clarify` and an `abstain` carry no pipeline, so their correctness lives in the
        // message and in what the model did NOT do. This also enforces the corpus rule that a
        // required fact must be projectable — you cannot verify what you cannot check.
        EXPECT_TRUE(admission_case.task.grader()
                        .grade(zetta::agent::response_solution_facts(
                            *admission_case.reference_action, admission_case.grounding))
                        .passed)
            << "case " << admission_case.task.id() << " reference must pass its own grader";
    }

    // The whole generated corpus ADMITS — reference passes, negative controls fail everywhere.
    EXPECT_TRUE(zetta::agent::admit_corpus(dataset.cases).admitted);
}

// A case that carries a pipeline MUST carry the tool calls that build it. The agent is a
// tool-calling harness: the model emits calls and the harness builds the pipeline from them, so a
// case with a graph and no calls trains the agent to produce a pipeline by doing nothing. That is
// not hypothetical — 91% of the v1 corpus's action cases were shaped exactly that way, and the
// checkpoint trained on it emitted an empty `tool_calls` for every request.
TEST(DatasetGenerator, EveryCaseWithAPipelineCarriesTheCallsThatBuildIt) {
    const auto generator = zetta::dataset::DatasetGeneratorFactory::builtin().create("base-agent");
    ASSERT_NE(generator, nullptr);

    zetta::dataset::DatasetGenSpec spec;
    spec.variants_per_subject = 2;
    const zetta::dataset::GeneratedDataset dataset = generator->generate(spec);

    int with_pipeline = 0;
    for (const zetta::agent::AdmissionCase& admission_case : dataset.cases) {
        ASSERT_TRUE(admission_case.reference_action.has_value());
        const zetta::agent::StudioAction& action = *admission_case.reference_action;
        // Scoped to `action`. An abstain deliberately carries a graph the gate scores while
        // building nothing — its graph is a grading artifact, not a pipeline — and answer /
        // clarify build nothing either. Only an action is a promise to construct something.
        if (action.kind != zetta::agent::InteractionKind::Action) continue;
        if (action.execution_graph.nodes().empty()) continue;
        ++with_pipeline;
        ASSERT_FALSE(action.tool_calls.empty())
            << "case " << admission_case.task.id()
            << " has a pipeline but no tool calls to build it";
        // And the calls must actually rebuild that pipeline — the harness's own path.
        EXPECT_EQ(zetta::agent::build_pipeline_from_tool_calls(action.tool_calls).nodes().size(),
                  action.execution_graph.nodes().size())
            << "case " << admission_case.task.id() << " calls must rebuild its pipeline";
    }
    EXPECT_GT(with_pipeline, 0) << "the fixture must contain pipeline-building cases";
}

TEST(DatasetGenerator, AutoSuggestProducesAGatedCorpus) {
    const auto generator =
        zetta::dataset::DatasetGeneratorFactory::builtin().create("auto-suggest");
    ASSERT_NE(generator, nullptr);
    EXPECT_EQ(generator->kind(), "auto-suggest");

    const zetta::dataset::GeneratedDataset dataset =
        generator->generate(zetta::dataset::DatasetGenSpec{});
    EXPECT_GE(dataset.subjects.size(), 5u);           // several data-state scenarios
    EXPECT_FALSE(dataset.cases.empty());
    // Discrimination: suggest the present, withhold the absent — the corpus admits.
    EXPECT_TRUE(zetta::agent::admit_corpus(dataset.cases).admitted);
}

}  // namespace
