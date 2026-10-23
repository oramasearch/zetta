// Native dataset generation (#873): the factory lists + creates generators, and the base-agent
// generator produces an admission-GATED corpus (blender-rl: reference passes, controls fail)
// with reference actions attached — the native replacement for the Python generators.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/agent/admission_gate.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/agent/response_facts.hpp"
#include "zetta/agent/solution_graph.hpp"
#include "zetta/dataset/base_agent_generator.hpp"
#include "zetta/dataset/dataset_generator.hpp"
#include "zetta/dataset/dataset_generator_factory.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/util/uuid.hpp"

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
                        .grade(zetta::agent::response_solution_facts(*admission_case.reference_action, admission_case.grounding,
                                            admission_case.history))
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

// #1398: the variant count is the one caller-controlled multiplier of a generator's work, so it is
// bounded per subject and, times the subject count, per corpus — before anything is expanded.
TEST(DatasetGenerator, BoundedVariantsChecksTheCeilingAndTheProductBeforeExpanding) {
    std::string reason;
    zetta::dataset::DatasetGenSpec spec;
    spec.variants_per_subject = 0;
    EXPECT_EQ(zetta::dataset::bounded_variants(spec, 40, &reason), 1) << "below 1 means 1";

    spec.variants_per_subject = zetta::dataset::kMaxVariantsPerSubject;
    EXPECT_EQ(zetta::dataset::bounded_variants(spec, 40, &reason),
              zetta::dataset::kMaxVariantsPerSubject);

    spec.variants_per_subject = zetta::dataset::kMaxVariantsPerSubject + 1;
    EXPECT_FALSE(zetta::dataset::bounded_variants(spec, 1, &reason));
    EXPECT_NE(reason.find("variants_per_subject"), std::string::npos) << reason;

    spec.variants_per_subject = std::numeric_limits<int>::max();
    EXPECT_FALSE(zetta::dataset::bounded_variants(spec, 1, &reason));

    // Within the per-subject ceiling, the corpus product is still capped, without overflowing.
    const std::size_t per_variant =
        zetta::dataset::kMaxExpandedCases / zetta::dataset::kMaxVariantsPerSubject;
    spec.variants_per_subject = zetta::dataset::kMaxVariantsPerSubject;
    EXPECT_TRUE(zetta::dataset::bounded_variants(spec, per_variant, &reason));
    EXPECT_FALSE(zetta::dataset::bounded_variants(spec, per_variant + 1, &reason));
    EXPECT_FALSE(zetta::dataset::bounded_variants(
        spec, std::numeric_limits<std::size_t>::max(), &reason));
}

TEST(DatasetGenerator, EveryBuiltinGeneratorRefusesAVariantCountOverTheCeiling) {
    for (const std::string kind : {"base-agent", "auto-suggest"}) {
        const auto generator = zetta::dataset::DatasetGeneratorFactory::builtin().create(kind);
        ASSERT_NE(generator, nullptr) << kind;
        for (const int variants :
             {zetta::dataset::kMaxVariantsPerSubject + 1, std::numeric_limits<int>::max()}) {
            zetta::dataset::DatasetGenSpec spec;
            spec.variants_per_subject = variants;
            const zetta::dataset::GeneratedDataset dataset = generator->generate(spec);
            EXPECT_FALSE(dataset.error.empty()) << kind << " x " << variants;
            EXPECT_TRUE(dataset.cases.empty()) << kind << " expanded a refused spec";
        }
    }
}

// The capability is how a workspace member reaches the generator: a variant count over the ceiling
// is refused before any case is built, and generation at the ceiling stays inside the case budget.
TEST(DatasetGenerator, GenerateDatasetRefusesAnUnboundedVariantCount) {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}, &replication};
    ASSERT_TRUE(node.start());
    node.ensure_builtin_roles();
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    auto trace = zetta::observability::make_root_trace();
    const auto generate = [&](const std::string& kind, const nlohmann::json& variants) {
        zetta::api::ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        nlohmann::json input{{"kind", kind}};
        if (!variants.is_null()) input["variants_per_subject"] = variants;
        return registry.call("generate_dataset", input, context);
    };

    for (const std::string kind : {"base-agent", "auto-suggest"}) {
        for (const nlohmann::json& variants :
             {nlohmann::json(zetta::dataset::kMaxVariantsPerSubject + 1),
              nlohmann::json(std::numeric_limits<int>::max()),
              nlohmann::json(std::numeric_limits<std::int64_t>::max())}) {
            const zetta::api::ApiResult refused = generate(kind, variants);
            EXPECT_FALSE(refused.ok) << kind << " x " << variants.dump();
            EXPECT_EQ(refused.error.code, "invalid_input") << kind << " x " << variants.dump();
        }

        const zetta::api::ApiResult ordinary = generate(kind, nullptr);
        ASSERT_TRUE(ordinary.ok) << ordinary.error.message;
        const zetta::api::ApiResult at_ceiling =
            generate(kind, zetta::dataset::kMaxVariantsPerSubject);
        ASSERT_TRUE(at_ceiling.ok) << at_ceiling.error.message;
        EXPECT_TRUE(at_ceiling.value.at("admitted").get<bool>()) << kind;
        // Only the per-subject variants multiply; the rest of the corpus is fixed by the engine.
        EXPECT_LE(at_ceiling.value.at("cases").get<std::size_t>() -
                      ordinary.value.at("cases").get<std::size_t>(),
                  zetta::dataset::kMaxExpandedCases)
            << kind;
    }
    node.stop();
}

}  // namespace
