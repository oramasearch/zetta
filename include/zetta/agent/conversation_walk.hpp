// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "zetta/agent/action.hpp"
#include "zetta/agent/end_state.hpp"

// Grade a THREAD, not a response (#983 acceptance 5).
//
// `response_solution_facts` grades one turn against the context it was given. It is the right
// question for a single reply and the wrong one for a conversation, because a conversation can
// fail while every individual turn looks fine: three well-formed, grounded, distinct replies that
// do not answer each other is the exact failure this exists to catch.
//
// **Why it exists.** The three-turn walk asserted one property — "no two consecutive replies are
// byte-identical" — chosen because that was the observed failure. It detects that failure and
// nothing else, so a model can pass by being differently wrong each turn. v16 did:
//
//     USER  create me a churn project
//     v16   Yes. Studio can create a new Project from your data and your needs.
//     USER  yes
//     v16   Yes. Studio can list_models to show which models are trained and what each costs.
//     USER  what?
//     v16   Sorry - plainly: list_models has over 100 entries, so anything that can make an
//           HTTP request can drive it.
//
// It PASSED. Turn 2 answers an approval with an unprompted claim about a tool nobody mentioned;
// turn 3 states a figure from nowhere. Distinctness is necessary and nowhere near sufficient.
//
// The four properties below are all decidable against facts the engine already knows, which is
// what keeps this a verifiable reward rather than a judgement call — the same reason the
// correctness gate is deterministic.
//
// See `docs/reports/base-agent-validation-2026-08-06.md` for the transcripts this is written
// against.
namespace zetta::agent {

// One exchange: what was asked, what came back, and the context the harness supplied when it
// asked. `grounding` may be empty — a turn with no retrieved context simply has fewer facts to
// check, not a free pass on the ones that remain.
struct WalkTurn {
    std::string ask;
    StudioAction response;
    std::string grounding;
};

// Is this ask a bare affirmation — a turn whose entire meaning is the turn before it?
//
// The vocabulary matches the corpus's `bare_turn` family deliberately. These are the exact strings
// people type; the grader recognizing a different set than the corpus teaches would score the
// agent on words it was never shown.
bool is_affirmation(std::string_view ask);

// The thread's facts. Each is emitted only when the thread contains a turn that could violate it,
// so a one-turn walk does not silently pass properties it never exercised:
//
//   no_consecutive_repeat  — no reply repeats the one before it, modulo whitespace and case.
//                            The original check. Kept: it caught a real regression three times.
//   affirmation_acted_on   — every bare affirmation ("yes", "go ahead") is answered by BUILDING
//                            something, not by describing what could be built. This is the
//                            propose-and-approve rung: "yes" after a proposal must act on THAT
//                            proposal.
//   identifiers_grounded   — every handle any reply names was named by the grounding, by the
//                            user, or by an earlier reply. The non-sequitur check.
//   numbers_grounded       — every figure any reply states appears in its context. This already
//                            existed and the walk never asserted it, which is how "over 100
//                            entries" passed.
//   walk_passes            — all of the above that were exercised.
EndState walk_facts(const std::vector<WalkTurn>& turns);

}  // namespace zetta::agent
