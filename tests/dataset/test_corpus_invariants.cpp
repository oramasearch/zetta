// SPDX-License-Identifier: MIT

// Corpus invariants — the rules that make the interaction corpus a well-designed, well-balanced
// TRAINING SET rather than merely a large pile of valid JSON.
//
// Why this file exists. A corpus can satisfy every schema, pass its admission gate, and still
// teach the wrong thing. It happened: 91% of the v1 corpus's `action` cases carried a real
// pipeline in `execution_graph` and an EMPTY `tool_calls`, because the case builder defaulted the
// two independently. Every case was individually valid. The DISTRIBUTION was the defect — and the
// checkpoint trained on it learned, correctly, that an action calls no tools and builds nothing.
//
// So these tests assert properties of the corpus AS A WHOLE: shape contracts that must hold for
// every case, learnability limits on what a target may ask a model to memorize, the
// anti-fabrication rule, and balance floors/ceilings that catch a family quietly dominating or
// collapsing. A rule here is cheap; discovering it from a trained checkpoint costs a training run.
//
// Related: docs/agent/cards/datasets/ (the data card states the intended distribution) and
// docs/agent/cards/models/ (the model card states what the shape is supposed to be).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "zetta/agent/action.hpp"
#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/interaction.hpp"
#include "zetta/agent/response_facts.hpp"
#include "zetta/agent/solution_graph.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/dataset/base_agent_generator.hpp"
#include "zetta/dataset/dataset_generator.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/dataset/product_claims.hpp"

namespace {

using zetta::agent::AdmissionCase;
using zetta::agent::InteractionKind;
using zetta::agent::StudioAction;
using zetta::agent::ToolCall;

// The shipped corpus, generated ONCE for the whole suite (generation is deterministic, so every
// test sees the identical corpus and a failure is always reproducible).
class CorpusInvariants : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        const zetta::dataset::BaseAgentDatasetGenerator generator;
        zetta::dataset::DatasetGenSpec spec;
        spec.held_out_fraction = 0.1;
        dataset_ = new zetta::dataset::GeneratedDataset(generator.generate(spec));
    }
    static void TearDownTestSuite() {
        delete dataset_;
        dataset_ = nullptr;
    }
    static const zetta::dataset::GeneratedDataset& dataset() { return *dataset_; }
    static const std::vector<AdmissionCase>& cases() { return dataset_->cases; }

    // The cases actually trained on (the canary is held out for measuring, never for fitting).
    static std::vector<AdmissionCase> training_cases() {
        const std::vector<AdmissionCase>& all = dataset_->cases;
        return std::vector<AdmissionCase>(all.begin(),
                                          all.end() - static_cast<long>(dataset_->held_out));
    }

    static bool builds_pipeline(const ToolCall& call) {
        return zetta::ir::step_kind_from_name(call.tool).has_value();
    }

  private:
    static zetta::dataset::GeneratedDataset* dataset_;
};

zetta::dataset::GeneratedDataset* CorpusInvariants::dataset_ = nullptr;

// =============================================================================================
// Shape contracts — properties every single case must satisfy.
// =============================================================================================

// THE regression test for the 91% bug. The agent is a tool-calling harness: the model emits calls
// and the HARNESS builds the pipeline from them. A case that declares a pipeline but no calls
// therefore teaches "produce this pipeline by doing nothing", which is not a thing the harness can
// execute. The calls must also genuinely REBUILD the declared pipeline, not merely exist.
TEST_F(CorpusInvariants, EveryActionThatDeclaresAPipelineCarriesTheCallsThatBuildIt) {
    int checked = 0;
    for (const AdmissionCase& admission_case : cases()) {
        ASSERT_TRUE(admission_case.reference_action.has_value())
            << admission_case.task.id() << " carries no action";
        const StudioAction& action = *admission_case.reference_action;
        // Scoped to `action`: an abstain deliberately carries a graph the gate scores while
        // building nothing (its graph is a grading artifact), and answer/clarify build nothing.
        if (action.kind != InteractionKind::Action) continue;
        if (action.execution_graph.nodes().empty()) continue;
        ++checked;
        ASSERT_FALSE(action.tool_calls.empty())
            << "case " << admission_case.task.id() << " declares a pipeline but calls nothing";
        EXPECT_EQ(zetta::agent::build_pipeline_from_tool_calls(action.tool_calls).nodes().size(),
                  action.execution_graph.nodes().size())
            << "case " << admission_case.task.id() << ": its calls must rebuild its pipeline";
    }
    EXPECT_GT(checked, 0) << "the corpus must contain pipeline-building cases at all";
}

// Every tool the corpus teaches the agent to call must be a tool that EXISTS — grounded in the
// live capability registry and the pipeline step kinds, not in the corpus's own vocabulary. A
// corpus is the only place an invented tool name can enter the model, and a model that calls a
// tool the platform does not have fails at the harness with no way to recover.
TEST_F(CorpusInvariants, EveryToolTheCorpusCallsIsARealTool) {
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    std::set<std::string> known;
    for (const auto& capability : registry.list()) known.insert(capability->name);
    ASSERT_GT(known.size(), 20u) << "the registry must actually be populated for this to mean much";

    std::map<std::string, int> unknown;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        for (const ToolCall& call : admission_case.reference_action->tool_calls) {
            const bool is_step = zetta::ir::step_kind_from_name(call.tool).has_value();
            if (!is_step && known.find(call.tool) == known.end()) ++unknown[call.tool];
        }
    }
    for (const auto& [tool, count] : unknown) {
        ADD_FAILURE() << "corpus calls '" << tool << "' " << count
                      << " times, but it is neither a pipeline step nor a registered capability";
    }
}

// Node ids must be unique within a case. A pipeline whose steps collide on one id does not
// compile — and this is not hypothetical: the first trained checkpoint emitted
// `11111111-1111-4111-8888-…` for every node, so its pipelines were structurally invalid.
TEST_F(CorpusInvariants, PipelineNodeIdsAreUniqueWithinEachCase) {
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        const auto& nodes = admission_case.reference_action->execution_graph.nodes();
        std::set<std::string> ids;
        for (const auto& node : nodes) ids.insert(node->id);
        EXPECT_EQ(ids.size(), nodes.size())
            << "case " << admission_case.task.id() << " has colliding pipeline node ids";
    }
}

// Only an ACTION may build. If an answer/clarify/abstain case carries pipeline-step calls, the
// corpus is teaching the agent to mutate the user's workspace while claiming it is just talking —
// the discrimination between "answer me" and "change my pipeline" is the whole point of `kind`.
TEST_F(CorpusInvariants, OnlyActionsBuildPipelines) {
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        const StudioAction& action = *admission_case.reference_action;
        if (action.kind == InteractionKind::Action) continue;
        for (const ToolCall& call : action.tool_calls) {
            EXPECT_FALSE(builds_pipeline(call))
                << "case " << admission_case.task.id() << " is a "
                << zetta::agent::to_string(action.kind) << " but calls the pipeline step '"
                << call.tool << "'";
        }
    }
}

// An abstain must build NOTHING at all — not even a read. Declining and then calling tools is the
// failure mode the abstain family exists to train against.
TEST_F(CorpusInvariants, AbstainsCallNothing) {
    int abstains = 0;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        if (admission_case.reference_action->kind != InteractionKind::Abstain) continue;
        ++abstains;
        EXPECT_TRUE(admission_case.reference_action->tool_calls.empty())
            << "case " << admission_case.task.id() << " abstains but still calls tools";
        EXPECT_FALSE(admission_case.reference_action->message.empty())
            << "case " << admission_case.task.id() << " abstains without saying why";
    }
    EXPECT_GT(abstains, 0) << "the corpus must train refusal at all";
}

// Arguments are always a JSON object. `null` or a bare scalar would train malformed calls that the
// harness must then defend against at runtime.
TEST_F(CorpusInvariants, EveryToolCallCarriesObjectArguments) {
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        for (const ToolCall& call : admission_case.reference_action->tool_calls) {
            EXPECT_TRUE(call.arguments.is_object())
                << "case " << admission_case.task.id() << " calls '" << call.tool
                << "' with non-object arguments: " << call.arguments.dump();
        }
    }
}

// Every case must actually say something. A response with an empty message renders as a blank
// turn in Studio no matter how correct its pipeline is.
TEST_F(CorpusInvariants, EveryCaseSaysSomethingToTheUser) {
    for (const AdmissionCase& admission_case : cases()) {
        ASSERT_TRUE(admission_case.reference_action.has_value());
        EXPECT_FALSE(admission_case.reference_action->message.empty())
            << "case " << admission_case.task.id() << " has an empty message";
    }
}

// =============================================================================================
// Learnability — limits on what a training target may ask a model to reproduce.
// =============================================================================================

// A training target must contain NO random identifiers. This is the single most expensive lesson
// in this corpus's history: node ids are random UUIDs, and because nlohmann orders keys
// alphabetically, `execution_graph` serialized FIRST — so a target spent most of its token budget
// on unlearnable random hex. There is no signal in a random id; all a model can extract is what a
// UUID looks like, which is exactly what the checkpoint learned to emit. The harness mints ids, so
// no target should ever contain one.
TEST_F(CorpusInvariants, TrainingTargetsCarryNoRandomIdentifiers) {
    const std::regex uuid(
        "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    int offenders = 0;
    for (const AdmissionCase& admission_case : cases()) {
        const std::string target = zetta::agent::render_action_example(admission_case).second;
        if (std::regex_search(target, uuid)) {
            if (++offenders <= 5) {
                ADD_FAILURE() << "case " << admission_case.task.id()
                              << " has a UUID in its TRAINING TARGET (unlearnable): "
                              << target.substr(0, 200);
            }
        }
    }
    EXPECT_EQ(offenders, 0) << offenders << " targets contain random identifiers";
}

// No target may dominate the token budget. A target far longer than its peers both starves the
// prompt at a fixed context length and skews the loss toward whichever case is longest.
TEST_F(CorpusInvariants, TrainingTargetsStayWithinABoundedSize) {
    std::size_t largest = 0;
    std::string worst;
    double total = 0.0;
    for (const AdmissionCase& admission_case : cases()) {
        const std::string target = zetta::agent::render_action_example(admission_case).second;
        total += static_cast<double>(target.size());
        if (target.size() > largest) {
            largest = target.size();
            worst = admission_case.task.id();
        }
    }
    const double mean = total / static_cast<double>(cases().size());
    // Generous ceilings — these catch a structural blowup (a graph leaking back into the target),
    // not ordinary variation between a one-line abstain and a six-step build.
    EXPECT_LT(largest, 4000u) << "largest target is case '" << worst << "' at " << largest
                              << " chars";
    EXPECT_LT(mean, 1200.0) << "mean target size " << mean << " chars is too large to train well";
}

// The renderer is THE shared train/serve path, so it must be a pure function of the case. If the
// same case rendered twice differed, training and serving could silently disagree.
TEST_F(CorpusInvariants, RenderingACaseIsDeterministic) {
    for (const AdmissionCase& admission_case : cases()) {
        const auto first = zetta::agent::render_action_example(admission_case);
        const auto second = zetta::agent::render_action_example(admission_case);
        ASSERT_EQ(first.first, second.first) << "prompt render is not deterministic";
        ASSERT_EQ(first.second, second.second) << "target render is not deterministic";
    }
}

// Every case must render a non-empty prompt AND target — an empty side is a case that trains
// nothing while still consuming a training slot.
TEST_F(CorpusInvariants, EveryCaseRendersATrainableExample) {
    for (const AdmissionCase& admission_case : cases()) {
        const auto [prompt, target] = zetta::agent::render_action_example(admission_case);
        EXPECT_FALSE(prompt.empty()) << "case " << admission_case.task.id() << " renders no prompt";
        EXPECT_FALSE(target.empty()) << "case " << admission_case.task.id() << " renders no target";
        EXPECT_NE(prompt.find(admission_case.task.description()), std::string::npos)
            << "case " << admission_case.task.id() << ": the ask must appear in its prompt";
    }
}

// =============================================================================================
// Anti-fabrication — the property that makes a grounded answer trustworthy.
// =============================================================================================

// Every number a reference message states must be a number the engine actually retrieved. This
// reuses the PRODUCTION rule (`response_solution_facts` -> `numbers_from_context`) rather than a
// second implementation, so the corpus is held to exactly the standard serving grades against.
// A fluent answer citing a plausible number that is not in the context is the worst failure this
// product can have, and the only place to prevent it is here.
TEST_F(CorpusInvariants, NoReferenceMessageCitesANumberItsGroundingDoesNotContain) {
    int grounded = 0;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        if (admission_case.grounding.empty()) continue;  // covered by the ungrounded rule below
        const zetta::agent::EndState facts = zetta::agent::response_solution_facts(*admission_case.reference_action, admission_case.grounding,
                                            admission_case.history);
        ++grounded;
        EXPECT_TRUE(facts.matches("numbers_from_context", true))
            << "case " << admission_case.task.id() << " fabricates a number.\n  message: "
            << admission_case.reference_action->message << "\n  grounding: "
            << admission_case.grounding.substr(0, 300);
    }
    EXPECT_GT(grounded, 0);
}

// The other half of the rule, which production cannot check: a message with NO grounding has
// nothing to ground against, so it must not state figures at all. `response_solution_facts`
// returns early on empty grounding, so an ungrounded reference citing "AUC 0.87" would sail
// through every gate and teach the agent to invent measures for a Project it has not read. A
// build-from-intent reply, or a product-knowledge answer, must stay qualitative.
TEST_F(CorpusInvariants, UngroundedMessagesStateNoFigures) {
    // A digit run touching a letter is a NAME (BM25F, GPT-4), not a stated figure — the same
    // distinction production's numeric_literals makes.
    const std::regex figure("(^|[^A-Za-z0-9])([0-9]{2,}|[0-9]+\\.[0-9]+)([^A-Za-z0-9]|$)");
    int ungrounded = 0;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        if (!admission_case.grounding.empty()) continue;
        ++ungrounded;
        EXPECT_FALSE(std::regex_search(admission_case.reference_action->message, figure))
            << "case " << admission_case.task.id()
            << " states a figure with no context to ground it: "
            << admission_case.reference_action->message;
    }
    EXPECT_GT(ungrounded, 0) << "the corpus must contain build-from-intent cases";
}

// A case that requires `grounded` must actually have been given something to ground in. Requiring
// groundedness with an empty context trains the model to claim it read facts that were not there.
TEST_F(CorpusInvariants, CasesThatRequireGroundingAreGivenGrounding) {
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference.matches("grounded", true)) continue;
        EXPECT_FALSE(admission_case.grounding.empty())
            << "case " << admission_case.task.id()
            << " requires a grounded answer but was given no context";
    }
}

// =============================================================================================
// Verifiability — the RLVR contract: the reference passes, and every control fails.
// =============================================================================================

TEST_F(CorpusInvariants, EveryReferencePassesItsOwnGrader) {
    for (const AdmissionCase& admission_case : cases()) {
        ASSERT_TRUE(admission_case.reference_action.has_value());
        EXPECT_TRUE(admission_case.task.grader()
                        .grade(zetta::agent::response_solution_facts(*admission_case.reference_action, admission_case.grounding,
                                            admission_case.history))
                        .passed)
            << "case " << admission_case.task.id() << " reference fails its own grader";
    }
}

// Discrimination is the product, not the pass. A case whose controls also pass teaches nothing,
// because the grader cannot tell right from wrong on it.
TEST_F(CorpusInvariants, EveryCaseHasNegativeControlsAndEveryControlFails) {
    for (const AdmissionCase& admission_case : cases()) {
        ASSERT_FALSE(admission_case.negative_controls.empty())
            << "case " << admission_case.task.id() << " has no negative control";
        for (std::size_t index = 0; index < admission_case.negative_controls.size(); ++index) {
            EXPECT_FALSE(
                admission_case.task.grader().grade(admission_case.negative_controls[index]).passed)
                << "case " << admission_case.task.id() << " control " << index
                << " PASSES — it does not discriminate";
        }
    }
}

// The corpus-wide gate must admit. Redundant with the two above by design: this is the check the
// generator itself runs, and it must never diverge from the per-case rules.
TEST_F(CorpusInvariants, TheWholeCorpusAdmits) {
    const zetta::agent::AdmissionResult result = zetta::agent::admit_corpus(cases());
    EXPECT_TRUE(result.admitted);
    for (std::size_t index = 0; index < result.failures.size() && index < 5; ++index) {
        ADD_FAILURE() << "admission failure: " << result.failures[index].task_id << " — "
                      << result.failures[index].detail;
    }
}

// =============================================================================================
// Balance — distribution properties. These are the rules a per-case check cannot express.
// =============================================================================================

// Every interaction mode must be represented well enough to learn. A mode at a handful of examples
// is a mode the model will not reliably enter, which collapses the discrimination `kind` exists
// for. The floor is deliberately low (1%) — it catches collapse, not imbalance.
TEST_F(CorpusInvariants, EveryInteractionKindIsRepresented) {
    std::map<std::string, int> by_kind;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        by_kind[std::string(zetta::agent::to_string(admission_case.reference_action->kind))]++;
    }
    const int total = static_cast<int>(cases().size());
    for (const char* kind : {"action", "answer", "clarify", "abstain"}) {
        const auto found = by_kind.find(kind);
        ASSERT_NE(found, by_kind.end()) << "kind '" << kind << "' is absent from the corpus";
        const double share = 100.0 * found->second / total;
        EXPECT_GE(share, 1.0) << "kind '" << kind << "' is only " << found->second << " cases ("
                              << share << "%) — too thin to learn";
    }
}

// ...and no single mode may swamp the rest. A corpus that is 90% one mode trains a model that
// answers everything that way regardless of what was asked.
TEST_F(CorpusInvariants, NoSingleInteractionKindDominates) {
    std::map<std::string, int> by_kind;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        by_kind[std::string(zetta::agent::to_string(admission_case.reference_action->kind))]++;
    }
    for (const auto& [kind, count] : by_kind) {
        const double share = 100.0 * count / static_cast<double>(cases().size());
        EXPECT_LE(share, 70.0) << "kind '" << kind << "' is " << share << "% of the corpus";
    }
}

// Messages must not be memorizable. If a handful of templates cover thousands of cases, the
// cheapest thing a model can learn is to emit a template verbatim — and that is precisely what the
// first checkpoint did, reproducing a corpus message word for word for an unrelated ask. Distinct
// messages relative to corpus size is the direct measure of that risk.
// The corpus-wide ratio above is necessary and not sufficient: a 10% floor is satisfiable
// while ANY SINGLE family is fully templated, because 15 other families dilute the average.
// That is not hypothetical — it is how #976 shipped. `capability_family` emits one sentence
// shape across every capability, the corpus-wide check stayed green, and a served checkpoint
// reproduced that sentence word for word for an ask the family did not own.
//
// So measure each family on its own. The family is the id's first segment.
// Every ask FORM the product accepts must be owned by a family. This is the check that
// would have caught all four agent-quality failures found by walking the product, none of
// which any diversity metric could see:
//
//   `list models`  (imperative)      -> landed on capability_discovery, was DESCRIBED
//   `hello`        (greeting)        -> landed on a pipeline build, emitted raw JSON
//   `thanks`       (acknowledgement) -> landed on tool_use, answered with a tool result
//
// An unowned form does not fail loudly. It produces a fluent, well-formed answer to a
// different question — which is the worst failure mode a corpus has, because every
// aggregate metric stays green through it.
// The ask-form invariant above asks "does SOME case use this wording". That is not enough for a
// BARE turn, because a bare turn can be present and still untrainable: "yes" with no history
// teaches the model to answer from the word alone, which is precisely the defect. So this asserts
// two stronger things the generator must keep true.
//
// It exists because the generator did NOT anticipate this class and nothing caught that. The
// corpus had a follow_up family, every case in it was a full sentence, and the second thing a real
// person types is one word. The result shipped: "yes" and "what?" returned a byte-identical reply.
// The corpus is a CLAIM SURFACE, and until this it was the only unguarded one. Every other
// property here is checked; the one that reaches a customer's ears in our own voice was not.
//
// A false claim in a meeting is made once, to one person, and can be corrected in the next call.
// A false claim in the corpus is made by the model, to every customer, forever — fluent,
// confident, grounded-looking, which is exactly what goes unnoticed. And it compounds: it becomes
// training data for the next checkpoint.
//
// The motivating instance: the corpus says "You can run Python in a Pipeline step"; the engine
// has PythonFrontend, documented as SIMULATED Python; and in the customer meeting the claim was
// made accurately. The meeting was more honest than the training data.
// The invariant that would have caught the v14 regression, and did not exist to.
//
// The `bare_turn` family's redirect reply opened with "Dropped that one." — a sentence appearing
// nowhere else in 16,239 cases. The trained model reached for it on "create me a churn project",
// a substantive request that is not a redirect, regressing a turn v13 had answered correctly.
// 180 cases (1.1% of the corpus) captured an ask form they were never meant for, purely by
// carrying a high-signal opening with nothing competing for the same surface.
//
// Every other invariant passed: the family carried history, trained two behaviours per word,
// admitted through the gate, and scored 85% on the canary. None of them look at DISTINCTIVENESS,
// which is what actually decided the outcome.
//
// So: an opening phrase used by one family must not be unique to it. Either other families use
// the same register, or the model has nothing to weigh it against.
// A DIAGNOSTIC, not an assertion — and the distinction is the finding.
//
// The `bare_turn` redirect opened with "Dropped that one." — a sentence appearing nowhere else in
// 16,239 cases. The trained model reached for it on "create me a churn project", a substantive
// request that is not a redirect, regressing a turn v13 answered correctly. 180 cases (1.1% of
// the corpus) captured an ask form they were never meant for, purely by carrying a high-signal
// opening with nothing competing for the same surface.
//
// Every other invariant passed. The family carried history, trained two behaviours per word,
// admitted through the gate, and scored 85% on the canary. None of them look at DISTINCTIVENESS,
// which is what actually decided the outcome.
//
// The obvious test — "no opening may be exclusive to one family" — FAILS ON LEGITIMATE CASES.
// Greetings should open distinctively ("Hello. I can…"); `tool_use` replies naturally begin
// "From `explain` on…". Exclusivity is not the defect. The defect is an opening that is exclusive
// AND semantically generic enough to attach to a different ask — "Dropped that one" is a decision
// phrase that fits anywhere, while "Hello." only answers a greeting. That distinction is not
// mechanically decidable here, and a test that fails on eight correct families is one the next
// person deletes.
//
// So this REPORTS the distribution rather than gating on it. Read it when a family regresses
// behaviour it did not touch: the suspect is usually an exclusive, generic opening.
TEST_F(CorpusInvariants, ReplyOpeningsAreReportedForDistinctiveness) {
    const auto opening = [](const std::string& message) {
        std::size_t cut = 0;
        int words = 0;
        while (cut < message.size() && words < 3) {
            if (message[cut] == ' ') ++words;
            ++cut;
        }
        return message.substr(0, cut);
    };

    std::map<std::string, std::set<std::string>> families_by_opening;
    std::map<std::string, std::size_t> uses;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        const std::string& message = admission_case.reference_action->message;
        if (message.size() < 12) continue;
        const std::string& id = admission_case.task.id();
        families_by_opening[opening(message)].insert(id.substr(0, id.find('/')));
        ++uses[opening(message)];
    }

    std::size_t exclusive_and_learnable = 0;
    for (const auto& [head, families] : families_by_opening) {
        if (families.size() == 1 && uses[head] >= 50) ++exclusive_and_learnable;
    }
    // No threshold to pass — the count is recorded so a jump is visible in the diff of a run.
    EXPECT_GE(families_by_opening.size(), exclusive_and_learnable)
        << "sanity only; this test reports rather than gates";
    std::cout << "[corpus] reply openings: " << families_by_opening.size() << " distinct, "
              << exclusive_and_learnable << " exclusive to one family and used >=50x\n";
}

TEST_F(CorpusInvariants, NoCaseAssertsACapabilityWeDoNotHave) {
    std::map<std::string, std::size_t> offences;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        const std::string offence =
            zetta::dataset::forbidden_claim_in(admission_case.reference_action->message);
        if (!offence.empty()) ++offences[offence];
    }

    std::string report;
    for (const auto& [phrase, count] : offences) {
        report += "\n  \"" + phrase + "\" in " + std::to_string(count) + " case(s)";
    }
    EXPECT_TRUE(offences.empty())
        << "these cases assert a capability the claims registry does not carry as shipped:"
        << report
        << "\n(a claim in the corpus is made by the model, in our voice, to every customer, on "
           "every turn — and it becomes training data for the next checkpoint)";
}

TEST_F(CorpusInvariants, EveryBareTurnCarriesTheTurnItDependsOn) {
    // Exact strings, not substrings — a bare turn is defined by being the WHOLE ask.
    const std::vector<std::string> bare = {"yes", "ok", "sure", "go ahead", "do it",
                                           "what?", "huh?", "no", "new one", "not that"};

    std::vector<std::string> missing;
    std::vector<std::string> without_history;
    for (const std::string& probe : bare) {
        bool seen = false;
        for (const AdmissionCase& admission_case : cases()) {
            if (admission_case.task.description() != probe) continue;
            seen = true;
            // A bare ask with no history is worse than an absent one: it is a training example
            // whose correct answer is not derivable from its own input.
            if (admission_case.history.empty()) without_history.push_back(probe);
        }
        if (!seen) missing.push_back(probe);
    }

    std::string missing_report;
    for (const std::string& form : missing) missing_report += "\"" + form + "\" ";
    EXPECT_TRUE(missing.empty())
        << "no case trains these bare turns: " << missing_report
        << "\n(a one-word turn is the second thing most people type; unowned, it is classified "
           "by surface form and every one of them gets the same reply)";

    std::string history_report;
    for (const std::string& form : without_history) history_report += "\"" + form + "\" ";
    EXPECT_TRUE(without_history.empty())
        << "these bare turns appear with NO history: " << history_report
        << "\n(the answer is not derivable from the ask, so the case teaches the model to "
           "respond to the word rather than to what it follows)";
}

// The pairing invariant. A bare word means different things after different turns, and a corpus
// that only ever pairs "yes" with "build it" teaches "yes -> build" — which is wrong exactly when
// it matters, because "yes" after an either/or question must still ask which one.
//
// Without this, the family above could be silently weakened to one case per word and every count
// in the coverage report would stay green.
TEST_F(CorpusInvariants, TheSameBareWordTrainsMoreThanOneBehavior) {
    std::map<std::string, std::set<std::string>> kinds_by_ask;
    for (const AdmissionCase& admission_case : cases()) {
        const std::string& ask = admission_case.task.description();
        // Bare = short enough that the ask carries no referent of its own.
        if (ask.size() > 12 || admission_case.history.empty()) continue;
        if (!admission_case.reference_action) continue;
        kinds_by_ask[ask].insert(std::string(zetta::agent::to_string(admission_case.reference_action->kind)));
    }

    // At least one bare word must be trained toward two different behaviors. Requiring it of
    // EVERY word would be wrong — "what?" legitimately has one correct shape.
    bool any_polysemous = false;
    for (const auto& [ask, kinds] : kinds_by_ask) {
        if (kinds.size() > 1) { any_polysemous = true; break; }
    }
    EXPECT_TRUE(any_polysemous)
        << "every bare turn in the corpus maps to exactly one behavior"
        << "\n(then the model can pass by memorizing the word — \"yes\" means build — and it will "
           "build when the previous turn offered a choice, which is the failure that matters)";
}

TEST_F(CorpusInvariants, EveryAskFormIsOwnedByAFamily) {
    // A representative ask per form the product actually accepts. If a form has no case
    // whose ask matches it, no family owns it and the nearest one will claim it.
    struct Form {
        const char* name;
        std::vector<std::string> probes;
    };
    const std::vector<Form> forms = {
        {"greeting", {"hello", "hi"}},
        {"acknowledgement", {"thanks", "thank you"}},
        {"imperative", {"list the models on this node", "show me the models"}},
        {"platform question", {"Can Orama", "How do I"}},
        {"ambiguous", {"Make it better.", "Fix this."}},
        {"out of scope", {"What's the capital of France?"}},
    };

    std::vector<std::string> asks;
    for (const AdmissionCase& admission_case : cases()) {
        asks.push_back(admission_case.task.description());
    }

    std::vector<std::string> unowned;
    for (const Form& form : forms) {
        bool owned = false;
        for (const std::string& probe : form.probes) {
            for (const std::string& ask : asks) {
                if (ask.find(probe) != std::string::npos) { owned = true; break; }
            }
            if (owned) break;
        }
        if (!owned) unowned.push_back(form.name);
    }

    std::string report;
    for (const std::string& form : unowned) report += form + " ";
    EXPECT_TRUE(unowned.empty())
        << "no family owns these ask forms: " << report
        << "\n(an unowned form is claimed by the nearest family, which answers a "
           "different question fluently — every aggregate metric stays green)";
}

TEST_F(CorpusInvariants, NoSingleFamilyIsOneTemplate) {
    std::map<std::string, std::set<std::string>> distinct_by_family;
    std::map<std::string, int> total_by_family;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        const std::string id = admission_case.task.id();
        const std::string family = id.substr(0, id.find('/'));
        distinct_by_family[family].insert(admission_case.reference_action->message);
        total_by_family[family]++;
    }

    // Deliberately lenient — this is a floor against "one template, a thousand cases",
    // not a style rule. A family below it is memorizable on its own however diverse the
    // corpus looks in aggregate.
    constexpr double kFloor = 0.05;
    std::string worst_family;
    double worst_ratio = 1.0;
    for (const auto& [family, distinct] : distinct_by_family) {
        const int total = total_by_family[family];
        if (total < 20) continue;  // too few cases for the ratio to mean anything
        const double ratio =
            static_cast<double>(distinct.size()) / static_cast<double>(total);
        if (ratio < worst_ratio) {
            worst_ratio = ratio;
            worst_family = family;
        }
    }

    EXPECT_GE(worst_ratio, kFloor)
        << "family '" << worst_family << "' has "
        << distinct_by_family[worst_family].size() << " distinct messages across "
        << total_by_family[worst_family] << " cases (" << 100.0 * worst_ratio
        << "%) — memorizable on its own, however diverse the corpus looks in aggregate";
}

TEST_F(CorpusInvariants, ReferenceMessagesAreNotMemorizable) {
    std::set<std::string> distinct;
    std::map<std::string, int> frequency;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        distinct.insert(admission_case.reference_action->message);
        frequency[admission_case.reference_action->message]++;
    }
    const double ratio = static_cast<double>(distinct.size()) / static_cast<double>(cases().size());
    EXPECT_GE(ratio, 0.10) << "only " << distinct.size() << " distinct messages across "
                           << cases().size() << " cases (" << 100.0 * ratio
                           << "%) — the model will memorize templates";

    // And no single message may cover a large slice on its own.
    std::string worst;
    int worst_count = 0;
    for (const auto& [message, count] : frequency) {
        if (count > worst_count) {
            worst_count = count;
            worst = message;
        }
    }
    const double worst_share = 100.0 * worst_count / static_cast<double>(cases().size());
    EXPECT_LE(worst_share, 10.0) << "one message covers " << worst_count << " cases ("
                                 << worst_share << "%): \"" << worst.substr(0, 120) << "\"";
}

// An ask that repeats must repeat against DIFFERENT context. This is the corpus's central design
// bet — hold the question fixed, vary the retrieved facts, and the example becomes unanswerable
// without reading them — so it is worth asserting directly rather than trusting.
//
// Note what this deliberately does NOT assert: that most asks are unique. This corpus is a cross
// product of contexts x asks x phrasings, so a low distinct-ask ratio is the design working, not a
// defect. Repetition against IDENTICAL context is the actual waste, and that is what this catches.
TEST_F(CorpusInvariants, RepeatedAsksAppearAgainstDifferentContext) {
    std::map<std::string, std::set<std::string>> grounding_by_ask;
    std::map<std::string, int> uses_by_ask;
    for (const AdmissionCase& admission_case : cases()) {
        if (admission_case.grounding.empty()) continue;
        grounding_by_ask[admission_case.task.description()].insert(admission_case.grounding);
        uses_by_ask[admission_case.task.description()]++;
    }
    ASSERT_FALSE(uses_by_ask.empty());
    int single_context_repeats = 0;
    for (const auto& [ask, uses] : uses_by_ask) {
        if (uses < 2) continue;
        if (grounding_by_ask[ask].size() < 2 && ++single_context_repeats <= 5) {
            ADD_FAILURE() << "ask \"" << ask.substr(0, 90) << "\" repeats " << uses
                          << " times against ONE context — duplication, not variation";
        }
    }
    EXPECT_EQ(single_context_repeats, 0);
}

// No two training examples may be byte-identical after rendering. An exact duplicate consumes a
// training slot, reweights whatever it duplicates, and — if it straddles the split — silently
// turns a canary measurement into a training measurement.
TEST_F(CorpusInvariants, NoTwoCasesRenderTheSameTrainingExample) {
    std::map<std::string, int> seen;
    for (const AdmissionCase& admission_case : cases()) {
        const auto [prompt, target] = zetta::agent::render_action_example(admission_case);
        seen[prompt + "\x1f" + target]++;
    }
    int duplicated = 0;
    int extra_slots = 0;
    for (const auto& [example, count] : seen) {
        if (count > 1) {
            ++duplicated;
            extra_slots += count - 1;
        }
    }
    EXPECT_EQ(duplicated, 0) << duplicated << " examples appear more than once, wasting "
                             << extra_slots << " of " << cases().size() << " training slots";
}

// Pipeline SHAPES must vary. If every build produces the same step sequence, the model learns one
// pipeline and stops reading the request — the failure that looks like competence in a demo and
// falls over on the first real dataset.
TEST_F(CorpusInvariants, PipelineShapesAreDiverse) {
    std::set<std::string> shapes;
    int builds = 0;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        const auto& nodes = admission_case.reference_action->execution_graph.nodes();
        if (nodes.empty()) continue;
        if (admission_case.reference_action->kind != InteractionKind::Action) continue;
        ++builds;
        std::string shape;
        for (const auto& node : nodes) {
            shape += zetta::ir::to_string(node->kind);
            shape += ">";
        }
        shapes.insert(shape);
    }
    ASSERT_GT(builds, 0);
    EXPECT_GE(shapes.size(), 4u) << "only " << shapes.size()
                                 << " distinct pipeline shapes across " << builds
                                 << " build cases — the agent will learn one pipeline";
}

// A Project's NAME must predict its pipeline. The grounding shows the name, so the model reads it;
// if the same label sometimes means a classifier and sometimes a forecaster, the only thing the
// corpus teaches is that the name is noise to be ignored.
//
// That is not hypothetical. Contexts once drew their name from one rotating list and their solution
// from another, so a Project called "Lifetime value forecast" required a gbdt classifier and one
// called "Churn risk" required regression. The checkpoint trained on it got `model_kind` wrong on
// 23 of 100 canary cases — it had been taught, correctly, that the name predicts nothing.
TEST_F(CorpusInvariants, AProjectNameAlwaysImpliesTheSamePipeline) {
    // Project label (without the trailing " #n" instance number) -> the pipeline shapes seen.
    std::map<std::string, std::set<std::string>> shapes_by_project;
    std::map<std::string, std::set<std::string>> kinds_by_project;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        const StudioAction& action = *admission_case.reference_action;
        if (action.kind != InteractionKind::Action || action.tool_calls.empty()) continue;
        if (admission_case.grounding.rfind("Project:", 0) != 0) continue;
        std::string label = admission_case.grounding.substr(
            9, admission_case.grounding.find('\n') - 9);
        if (const std::size_t hash = label.rfind(" #"); hash != std::string::npos) {
            label = label.substr(0, hash);
        }
        std::string shape;
        for (const ToolCall& call : action.tool_calls) {
            shape += call.tool;
            shape += ">";
            if (call.arguments.is_object()) {
                const auto kind = call.arguments.find("model_kind");
                if (kind != call.arguments.end() && kind->is_string()) {
                    kinds_by_project[label].insert(kind->get<std::string>());
                }
            }
        }
        shapes_by_project[label].insert(shape);
    }
    ASSERT_FALSE(shapes_by_project.empty()) << "no grounded action cases to check";

    for (const auto& [label, kinds] : kinds_by_project) {
        EXPECT_LE(kinds.size(), 1u)
            << "Project \"" << label << "\" is trained with " << kinds.size()
            << " different model kinds — its name predicts nothing";
    }
    // Shapes may legitimately vary a little (a modify request adds a step), but not wildly: a
    // handful of variants is a Project being edited, a dozen is a label meaning nothing.
    for (const auto& [label, shapes] : shapes_by_project) {
        EXPECT_LE(shapes.size(), 4u)
            << "Project \"" << label << "\" is trained with " << shapes.size()
            << " different pipeline shapes";
    }
}

// A follow-up must carry the turn it follows. "Why?" and "Add calibration to that" have no
// referent in themselves — a case whose history was lost trains as a standalone question, silently,
// looking exactly like a well-formed case. That is how every turn came to train as a FIRST turn.
TEST_F(CorpusInvariants, FollowUpCasesCarryTheTurnTheyFollow) {
    int follow_ups = 0;
    for (const AdmissionCase& admission_case : cases()) {
        if (admission_case.task.id().rfind("follow/", 0) != 0) continue;
        ++follow_ups;
        ASSERT_FALSE(admission_case.history.empty())
            << "case " << admission_case.task.id() << " is a follow-up with no history";
        for (const zetta::agent::PriorTurn& turn : admission_case.history) {
            EXPECT_FALSE(turn.ask.empty());
            EXPECT_FALSE(turn.reply.empty());
            // The reply is the agent's MESSAGE, never the envelope — re-showing JSON would teach
            // the model that the envelope is part of a conversation.
            EXPECT_EQ(turn.reply.find("\"tool_calls\""), std::string::npos)
                << "case " << admission_case.task.id() << " replays an action envelope as dialogue";
        }
    }
    EXPECT_GT(follow_ups, 0) << "the corpus must train follow-ups at all";
}

// An ACTION corpus must teach more than one pipeline. This is the rule that would have predicted
// the v3 checkpoint's behaviour before a GPU was booked: 92% of its action cases were just two
// shapes, so the model learned one pipeline and emitted it for churn, lead scoring and collections
// alike — passing any grader whose facts that shape happens to satisfy and failing the rest.
//
// Measured on the ordered step kinds PLUS the methodology, not on the rest of the attributes.
//
// The methodology used to be a config value, and this invariant deliberately ignored it. Since
// `model_kind` rides the model step (B1a) that is no longer true: a forecast and a churn model
// are not the same structure with a different setting, because the correctness gate accepts
// different things from each — `TemporalValidationCheck` requires temporal validation of one and
// refuses calibration on it, and `MethodologyCheck` refuses a methodology the engine cannot
// execute. Two cases whose steps match but whose methodologies differ teach genuinely different
// pipelines and must count as such.
//
// Everything else stays excluded: a threshold or a column name really is a config value.
TEST_F(CorpusInvariants, ActionCasesTeachMoreThanOnePipelineShape) {
    std::map<std::string, int> shapes;
    int actions = 0;
    for (const AdmissionCase& admission_case : cases()) {
        if (!admission_case.reference_action) continue;
        const StudioAction& action = *admission_case.reference_action;
        if (action.kind != InteractionKind::Action) continue;
        std::string shape;
        for (const ToolCall& call : action.tool_calls) {
            shape += call.tool;
            shape += ">";
        }
        for (const auto& node : action.execution_graph.nodes()) {
            if (node->kind != zetta::ir::StepKind::Model || !node->attributes.is_object()) continue;
            if (const auto kind = node->attributes.find("model_kind");
                kind != node->attributes.end() && kind->is_string()) {
                shape += "[" + kind->get<std::string>() + "]";
            }
        }
        if (shape.empty()) continue;
        ++actions;
        shapes[shape]++;
    }
    ASSERT_GT(actions, 0);
    int most = 0;
    std::string dominant;
    for (const auto& [shape, count] : shapes) {
        if (count > most) {
            most = count;
            dominant = shape;
        }
    }
    const double share = 100.0 * most / actions;
    EXPECT_LE(share, 45.0) << "one pipeline shape is " << share << "% of all action cases ("
                           << dominant << ") — the agent will learn it and emit it for everything";
    EXPECT_GE(shapes.size(), 12u)
        << "only " << shapes.size() << " distinct pipeline shapes across " << actions
        << " action cases";
}

// The targets must carry enough information to be worth learning. A corpus whose targets collapse
// onto a handful of strings drives training loss to ~0 while teaching nothing that generalizes —
// v3 reached a loss of 0.0001 and 47% grader pass on held-out cases, which is what memorizing a
// low-entropy target set looks like from the inside. Low loss is not evidence of learning; this is
// the check that says whether there was anything to learn.
TEST_F(CorpusInvariants, TargetsCarryEnoughInformationToBeWorthLearning) {
    std::map<std::string, int> targets;
    for (const AdmissionCase& admission_case : cases()) {
        targets[zetta::agent::render_action_example(admission_case).second]++;
    }
    const double total = static_cast<double>(cases().size());
    double entropy = 0.0;
    for (const auto& [target, count] : targets) {
        const double probability = count / total;
        entropy -= probability * std::log2(probability);
    }
    EXPECT_GE(entropy, 9.5) << "target entropy is only " << entropy << " bits across "
                            << targets.size() << " distinct targets — a model can memorize this";
}

// Every grounded family must draw on many different contexts. Context variation is the
// anti-fabrication multiplier: a model that memorized answers instead of reading the supplied
// facts fails as soon as the facts move. If a family drew on one or two contexts, its answers
// could be learned outright.
//
// Measured as distinct contexts per family rather than corpus-wide distinct/total, because the
// latter is structurally capped by the cross product (cases grow with asks x phrasings while
// groundings grow only with contexts) and so says nothing about whether variation exists.
TEST_F(CorpusInvariants, GroundedFamiliesDrawOnManyDifferentContexts) {
    std::map<std::string, std::set<std::string>> contexts_by_family;
    for (const AdmissionCase& admission_case : cases()) {
        if (admission_case.grounding.empty()) continue;
        const std::string id = admission_case.task.id();
        const std::size_t cut = id.find_first_of("/#:");
        contexts_by_family[cut == std::string::npos ? id : id.substr(0, cut)].insert(
            admission_case.grounding);
    }
    ASSERT_FALSE(contexts_by_family.empty());
    for (const auto& [family, distinct] : contexts_by_family) {
        EXPECT_GE(distinct.size(), 3u)
            << "family '" << family << "' is grounded in only " << distinct.size()
            << " distinct context(s) — its answers can be memorized";
    }
}

// No single subject may swamp the corpus. Churn is the flagship use case, which is exactly why it
// is the one at risk of crowding everything else out.
TEST_F(CorpusInvariants, NoSingleSubjectDominatesTheCorpus) {
    ASSERT_FALSE(dataset().subjects.empty());
    std::map<std::string, int> by_subject;
    for (const AdmissionCase& admission_case : cases()) {
        // Task ids are subject-prefixed; the segment before the first '/' or '#' is the subject.
        const std::string id = admission_case.task.id();
        const std::size_t cut = id.find_first_of("/#:");
        by_subject[cut == std::string::npos ? id : id.substr(0, cut)]++;
    }
    for (const auto& [subject, count] : by_subject) {
        const double share = 100.0 * count / static_cast<double>(cases().size());
        EXPECT_LE(share, 50.0) << "subject '" << subject << "' is " << share
                               << "% of the corpus — it crowds out the rest";
    }
}

// =============================================================================================
// Split hygiene — the canary only measures if the model has not seen it.
// =============================================================================================

// The canary must be held out and non-empty, or every "held-out" number reported is really a
// training number.
TEST_F(CorpusInvariants, TheCanaryIsHeldOutAndNonEmpty) {
    EXPECT_GT(dataset().held_out, 0u) << "no canary was reserved — nothing can be measured";
    EXPECT_LT(dataset().held_out, cases().size()) << "the canary must not consume the corpus";
}

// No canary ask may also appear in training. An overlapping canary reports memorization as
// generalization, which is the one measurement error that makes every other number meaningless.
TEST_F(CorpusInvariants, TheCanaryDoesNotOverlapTraining) {
    const std::vector<AdmissionCase> train = training_cases();
    std::set<std::string> trained;
    for (const AdmissionCase& admission_case : train) {
        trained.insert(admission_case.task.description() + "\n" + admission_case.grounding);
    }
    int leaked = 0;
    for (std::size_t index = train.size(); index < cases().size(); ++index) {
        const AdmissionCase& canary = cases()[index];
        if (trained.count(canary.task.description() + "\n" + canary.grounding) > 0) {
            if (++leaked <= 5) {
                ADD_FAILURE() << "canary case " << canary.task.id()
                              << " also appears in training — the canary measures memorization";
            }
        }
    }
    EXPECT_EQ(leaked, 0) << leaked << " canary cases leak into training";
}

}  // namespace
