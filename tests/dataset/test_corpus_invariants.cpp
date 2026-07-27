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
        const zetta::agent::EndState facts = zetta::agent::response_solution_facts(
            *admission_case.reference_action, admission_case.grounding);
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
    const std::regex figure("[0-9]{2,}|[0-9]+\\.[0-9]+");
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
                        .grade(zetta::agent::response_solution_facts(
                            *admission_case.reference_action, admission_case.grounding))
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
