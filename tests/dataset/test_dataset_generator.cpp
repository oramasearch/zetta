// Native dataset generation (#873): the factory lists + creates generators, and the base-agent
// generator produces an admission-GATED corpus (blender-rl: reference passes, controls fail)
// with reference actions attached — the native replacement for the Python generators.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "zetta/agent/admission_gate.hpp"
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

    // Two subjects (churn + search) × 2 variants = 4 gated cases, each with a reference action.
    EXPECT_EQ(dataset.subjects.size(), 2u);
    ASSERT_EQ(dataset.cases.size(), 4u);
    for (const zetta::agent::AdmissionCase& admission_case : dataset.cases) {
        ASSERT_TRUE(admission_case.reference_action.has_value()) << "each case carries its action";
        EXPECT_TRUE(admission_case.task.grader()
                        .grade(zetta::agent::graph_solution_facts(
                            admission_case.reference_action->execution_graph))
                        .passed);
    }

    // The whole generated corpus ADMITS — reference passes, negative controls fail everywhere.
    EXPECT_TRUE(zetta::agent::admit_corpus(dataset.cases).admitted);
}

}  // namespace
