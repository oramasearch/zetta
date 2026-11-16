// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iostream>

#include <string>
#include <vector>

#include "zetta/agent/action.hpp"
#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/base_prompt.hpp"
#include "zetta/tokenizer/bpe.hpp"
#include "zetta/agent/chat.hpp"
#include "zetta/agent/corpus_io.hpp"
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
        // Including the conversation so far: a multi-turn case must be byte-identical too, or
        // follow-ups train under one frame and serve under another — the exact skew this test
        // exists to catch, and it did catch it when history was added to the renderer.
        const std::string served_prompt = zetta::agent::build_chat_prompt(
            base_agent(), admission_case.task.description(), admission_case.grounding,
            /*tool_specs=*/{}, admission_case.history);
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

TEST(InteractionContract, GroundingSurvivesTheCorpusRoundTrip) {
    // Half of a grounded-QA example is its retrieved context. A round trip that dropped it
    // would present the model with "answer from these facts" and no facts — training it to
    // answer from priors, which is the exact fabrication the corpus exists to remove. This
    // silently produced a degraded corpus before `grounding` was added to the serialization.
    const zetta::dataset::BaseAgentDatasetGenerator generator;
    const zetta::dataset::GeneratedDataset dataset = generator.generate({});

    const std::string encoded = zetta::agent::dump_corpus_jsonl(dataset.cases);
    std::string error;
    const std::optional<std::vector<zetta::agent::AdmissionCase>> reloaded =
        zetta::agent::load_corpus_jsonl(encoded, &error);
    ASSERT_TRUE(reloaded.has_value()) << error;
    ASSERT_EQ(reloaded->size(), dataset.cases.size());

    std::size_t grounded_cases = 0;
    for (std::size_t index = 0; index < dataset.cases.size(); ++index) {
        EXPECT_EQ((*reloaded)[index].grounding, dataset.cases[index].grounding)
            << "grounding lost on " << dataset.cases[index].task.id();
        if (!dataset.cases[index].grounding.empty()) ++grounded_cases;
        // And the rendered training example must be identical after the round trip.
        EXPECT_EQ(zetta::agent::render_action_example((*reloaded)[index]).first,
                  zetta::agent::render_action_example(dataset.cases[index]).first);
    }
    EXPECT_GT(grounded_cases, 0u) << "no case carries context: nothing is being grounded";
}

// Env-gated corpus export (the repo's ZETTA_WRITE_* pattern): writes the gated corpus as JSONL
// for the SFT driver to consume. Never a build/CI dependency — skipped unless asked for.
TEST(InteractionContract, WriteCorpusJsonl) {
    const char* out = std::getenv("ZETTA_CORPUS_OUT");
    if (out == nullptr) GTEST_SKIP() << "set ZETTA_CORPUS_OUT to write the corpus";

    const zetta::dataset::BaseAgentDatasetGenerator generator;
    zetta::dataset::DatasetGenSpec spec;
    spec.held_out_fraction = 0.1;
    const zetta::dataset::GeneratedDataset dataset = generator.generate(spec);
    ASSERT_TRUE(zetta::agent::admit_corpus(dataset.cases).admitted);

    // Training reads cases[0 .. size-held_out); the canary is the remainder (the convention on
    // GeneratedDataset). Write the training split — the canary is for measuring, not fitting.
    const std::size_t train_count = dataset.cases.size() - dataset.held_out;
    const std::vector<zetta::agent::AdmissionCase> train(
        dataset.cases.begin(),
        dataset.cases.begin() + static_cast<std::ptrdiff_t>(train_count));
    const std::vector<zetta::agent::AdmissionCase> canary(
        dataset.cases.begin() + static_cast<std::ptrdiff_t>(train_count), dataset.cases.end());

    std::ofstream(std::string(out)) << zetta::agent::dump_corpus_jsonl(train);
    std::ofstream(std::string(out) + ".canary") << zetta::agent::dump_corpus_jsonl(canary);
    std::cout << "\n  wrote " << train.size() << " training cases to " << out << "\n"
              << "  wrote " << canary.size() << " canary cases to " << out << ".canary\n";
}


// Diagnostic: how much of a training example is the PROMPT? The SFT driver concatenates prompt and
// target and scores cross-entropy over every position, so a constant system prompt shared by all
// 6,030 examples gets memorized in a few steps and then dominates the reported loss. If the prompt
// is most of the sequence, the headline loss says far more about template memorization than about
// whether the model learned to answer.
TEST(InteractionContract, ReportPromptToTargetTokenSplit) {
    const char* model_dir = std::getenv("ZETTA_QWEN_DIR");
    if (model_dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR for the tokenizer";
    std::string error;
    const std::optional<zetta::tokenizer::BpeTokenizer> tokenizer =
        zetta::tokenizer::BpeTokenizer::from_directory(model_dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;

    const zetta::dataset::BaseAgentDatasetGenerator generator;
    const zetta::dataset::GeneratedDataset dataset = generator.generate({});

    std::size_t prompt_total = 0, target_total = 0, counted = 0;
    std::size_t system_prompt_tokens =
        tokenizer->encode(std::string(zetta::agent::base_agent_system_prompt())).size();
    for (const zetta::agent::AdmissionCase& c : dataset.cases) {
        if (counted >= 400) break;
        const auto [prompt, target] = zetta::agent::render_action_example(c);
        prompt_total += tokenizer->encode(prompt).size();
        target_total += tokenizer->encode(target).size();
        ++counted;
    }
    const double prompt_mean = static_cast<double>(prompt_total) / counted;
    const double target_mean = static_cast<double>(target_total) / counted;
    std::cout << "\n  system prompt alone: " << system_prompt_tokens << " tokens\n"
              << "  mean prompt: " << prompt_mean << "  mean target: " << target_mean << "\n"
              << "  PROMPT SHARE OF LOSS: "
              << 100.0 * prompt_mean / (prompt_mean + target_mean) << "%\n";
}

// A follow-up must resolve against what was actually said. Without history every turn is a FIRST
// turn: "list all of those options" has no referent, and three different follow-ups produce the
// same reply because the model sees three unrelated opening messages.
TEST(InteractionContract, HistoryAppearsInThePromptAndServingMatchesTraining) {
    const std::vector<zetta::agent::PriorTurn> history = {
        {"What can I do with Orama Studio?", "I can build churn, lead scoring, or forecasting."}};

    const std::string trained = zetta::agent::render_interaction_prompt(
        zetta::agent::base_agent_system_prompt(), /*grounding=*/"",
        "List all of those options.", history);

    EXPECT_NE(trained.find("What can I do with Orama Studio?"), std::string::npos)
        << "the earlier ask must be visible";
    EXPECT_NE(trained.find("I can build churn, lead scoring, or forecasting."), std::string::npos)
        << "the earlier reply must be visible";
    // The reply shown is the MESSAGE, never the envelope — re-showing JSON would teach the model
    // that the envelope is part of the dialogue. Checked on the history BLOCK, because the system
    // prompt legitimately names tool_calls when it describes the response shape.
    const std::size_t block = trained.find("Earlier in this conversation");
    ASSERT_NE(block, std::string::npos);
    const std::string history_block = trained.substr(block, trained.find("List all of those") - block);
    EXPECT_EQ(history_block.find("\"tool_calls\""), std::string::npos);

    // THE contract: what serving sends is byte-identical to what training rendered.
    zetta::agent::AgentRecord agent;
    agent.name = "test";
    agent.model = "/checkpoints/base";
    const std::string served =
        zetta::agent::build_chat_prompt(agent, "List all of those options.", /*grounding=*/"",
                                        /*tool_specs=*/{}, history);
    EXPECT_EQ(served, trained) << "a multi-turn prompt must not differ between training and serving";
}

// No history renders exactly as before, so every single-turn case is unchanged.
TEST(InteractionContract, NoHistoryRendersIdenticallyToBefore) {
    const std::string without = zetta::agent::render_interaction_prompt(
        zetta::agent::base_agent_system_prompt(), "", "Set up churn.");
    const std::string empty = zetta::agent::render_interaction_prompt(
        zetta::agent::base_agent_system_prompt(), "", "Set up churn.", {});
    EXPECT_EQ(without, empty);
    EXPECT_EQ(without.find("Earlier in this conversation"), std::string::npos);
}

}  // namespace
