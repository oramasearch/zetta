#include <gtest/gtest.h>

#include <iostream>

#include <string>
#include <vector>

#include "zetta/agent/action.hpp"
#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/base_prompt.hpp"
#include "zetta/agent/chat.hpp"
#include "zetta/agent/interaction.hpp"
#include "zetta/agent/solution_graph.hpp"
#include "zetta/dataset/base_agent_generator.hpp"
#include "zetta/dataset/interaction_families.hpp"

// The interaction contract (#918) — the tests that keep training and serving honest.
//
// The load-bearing one is TrainingPromptIsByteIdenticalToServedPrompt. Its absence is what let
// the harness drift from the corpus: training baked in `base_agent_system_prompt()` while
// serving prepended a per-agent prompt and appended "User:"/"Assistant:" turn labels that appear
// in ZERO training examples. Every downstream symptom (raw JSON replies, ignored instructions,
// invented numbers) followed from that. This test makes the drift impossible to reintroduce
// silently.
namespace {

using zetta::agent::AdmissionCase;
using zetta::agent::AgentRecord;
using zetta::agent::InteractionKind;

AgentRecord base_agent() {
    AgentRecord agent;
    agent.id = "test-agent";
    agent.name = "Base agent";
    agent.model = "/nonexistent/checkpoint";  // never loaded: we only build the prompt
    return agent;
}

TEST(InteractionContract, TrainingPromptIsByteIdenticalToServedPrompt) {
    // Walk real generated cases, not a hand-made one, so the assertion covers every family.
    const zetta::dataset::BaseAgentDatasetGenerator generator;
    const zetta::dataset::GeneratedDataset dataset = generator.generate({});
    ASSERT_FALSE(dataset.cases.empty());

    for (const AdmissionCase& admission_case : dataset.cases) {
        const auto [training_prompt, target] = zetta::agent::render_action_example(admission_case);
        // What the SERVING harness would build for the same ask + retrieved context.
        const std::string served_prompt = zetta::agent::build_chat_prompt(
            base_agent(), admission_case.task.description(), admission_case.grounding);
        EXPECT_EQ(training_prompt, served_prompt)
            << "train/serve prompt skew on case " << admission_case.task.id();
        EXPECT_FALSE(target.empty());
    }
}

TEST(InteractionContract, ServedPromptCarriesTheTrainedSystemPromptByDefault) {
    const std::string prompt = zetta::agent::build_chat_prompt(base_agent(), "Predict churn.");
    EXPECT_NE(prompt.find(zetta::agent::base_agent_system_prompt()), std::string::npos);
    // The turn labels that never appeared in training must never appear in serving.
    EXPECT_EQ(prompt.find("\nUser: "), std::string::npos);
    EXPECT_EQ(prompt.find("\nAssistant:"), std::string::npos);
}

TEST(InteractionContract, GroundingIsPlacedByTheSharedRendererNotTheCallSite) {
    const std::string grounding = "Measure: AUC 0.880\n";
    const std::string prompt = zetta::agent::build_chat_prompt(base_agent(), "How good is it?",
                                                               grounding);
    EXPECT_NE(prompt.find(grounding), std::string::npos);
    EXPECT_NE(prompt.find(zetta::agent::grounding_preamble()), std::string::npos);
    // Context comes BEFORE the ask, so the answer is about what was retrieved.
    EXPECT_LT(prompt.find(grounding), prompt.find("How good is it?"));
}

TEST(InteractionContract, ModeRoundTripsThroughTheEnvelope) {
    for (const InteractionKind kind : {InteractionKind::Action, InteractionKind::Answer,
                                       InteractionKind::Clarify, InteractionKind::Abstain}) {
        zetta::agent::StudioAction action;
        action.kind = kind;
        action.message = "hello";
        const std::string encoded = zetta::agent::to_json(action).dump();
        const std::optional<zetta::agent::StudioAction> parsed =
            zetta::agent::parse_studio_action(encoded);
        ASSERT_TRUE(parsed.has_value()) << encoded;
        EXPECT_EQ(parsed->kind, kind);
        EXPECT_EQ(parsed->message, "hello");
    }
}

TEST(InteractionContract, UnknownModeIsABoundaryErrorNotASilentDefault) {
    std::string reason;
    const nlohmann::json document = {{"kind", "improvise"}, {"message", "hi"}};
    EXPECT_FALSE(zetta::agent::action_from_json(document, &reason).has_value());
    EXPECT_NE(reason.find("kind"), std::string::npos);
}

TEST(InteractionContract, PreDiscriminatorDocumentsStillParseAsActions) {
    // Back-compat: every action document written before the mode existed must keep its meaning.
    const nlohmann::json document = {{"message", "Here's the pipeline."},
                                     {"tool_calls", nlohmann::json::array()}};
    const std::optional<zetta::agent::StudioAction> parsed =
        zetta::agent::action_from_json(document, nullptr);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, InteractionKind::Action);
}

TEST(InteractionContract, EveryFamilyAdmitsAndCarriesDiscriminatingControls) {
    for (const zetta::dataset::InteractionFamily& family :
         zetta::dataset::interaction_families(6, 4)) {
        ASSERT_FALSE(family.cases.empty()) << family.name << " produced no cases";
        const zetta::agent::AdmissionResult result =
            zetta::agent::admit_corpus(family.cases);
        std::string detail;
        for (const zetta::agent::AdmissionFailure& failure : result.failures) {
            detail += std::string(zetta::agent::to_string(failure.kind)) + " on " +
                      failure.task_id + ": " + failure.detail + "\n";
        }
        EXPECT_TRUE(result.admitted) << family.name << " failed admission:\n" << detail;
        for (const AdmissionCase& admission_case : family.cases) {
            EXPECT_FALSE(admission_case.negative_controls.empty())
                << admission_case.task.id() << " has no negative control";
        }
    }
}

TEST(InteractionContract, GroundedQaVariesTheContextSoAnswersCannotBeMemorized) {
    // The anti-fabrication property: same question, different retrieved facts, different answer.
    // If two contexts produced the same grounding, the corpus would be teaching recall.
    const std::vector<zetta::dataset::ProjectContext> contexts =
        zetta::dataset::synthetic_project_contexts(12);
    std::vector<std::string> renderings;
    for (const zetta::dataset::ProjectContext& context : contexts) {
        renderings.push_back(
            zetta::dataset::render_grounding(context, zetta::dataset::GroundingView::Results));
    }
    for (std::size_t left = 0; left < renderings.size(); ++left) {
        EXPECT_FALSE(renderings[left].empty());
        for (std::size_t right = left + 1; right < renderings.size(); ++right) {
            EXPECT_NE(renderings[left], renderings[right])
                << "contexts " << left << " and " << right << " are indistinguishable";
        }
    }
}

TEST(InteractionContract, NoSyntheticModelScoresAPerfectMeasure) {
    // A measure of 1.000 reads as leakage to the technical buyer this is sold to; the corpus
    // must never teach the agent to present one as a success.
    for (const zetta::dataset::ProjectContext& context :
         zetta::dataset::synthetic_project_contexts(60)) {
        EXPECT_LT(context.auc, 0.99) << context.project;
        EXPECT_LT(context.accuracy, 0.99) << context.project;
    }
}

TEST(InteractionContract, CorpusIsLargeEnoughAndEveryCaseAdmits) {
    // The corpus coverage report (#918). A small corpus is the limitation the data card has
    // always admitted ("broad, not deep"); this asserts we have actually fixed it, and prints
    // the per-family breakdown that goes into the validation report.
    const zetta::dataset::BaseAgentDatasetGenerator generator;
    zetta::dataset::DatasetGenSpec spec;
    spec.held_out_fraction = 0.1;  // a canary is required, so ask for one
    const zetta::dataset::GeneratedDataset dataset = generator.generate(spec);

    std::cout << "\n  corpus: " << dataset.cases.size() << " cases across "
              << dataset.subjects.size() << " subjects (" << dataset.held_out
              << " held out as canary)\n";
    for (const zetta::dataset::InteractionFamily& family :
         zetta::dataset::interaction_families(30, 4)) {
        std::cout << "    " << family.name << ": " << family.cases.size() << "\n";
    }

    EXPECT_GE(dataset.cases.size(), 5000u)
        << "the corpus must be sizeable enough to train a stable output contract";
    EXPECT_GT(dataset.held_out, 0u) << "no canary: memorization would go unmeasured";

    const zetta::agent::AdmissionResult result = zetta::agent::admit_corpus(dataset.cases);
    std::string detail;
    for (const zetta::agent::AdmissionFailure& failure : result.failures) {
        detail += std::string(zetta::agent::to_string(failure.kind)) + " on " + failure.task_id +
                  ": " + failure.detail + "\n";
    }
    EXPECT_TRUE(result.admitted) << "corpus failed admission:\n" << detail;
}

}  // namespace
