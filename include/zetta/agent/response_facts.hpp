#pragma once

#include <string>

#include "zetta/agent/action.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/agent/interaction.hpp"

// Project a RESPONSE into gradeable facts (#918) — the verifiable-reward primitive for every
// interaction mode, and the counterpart of `graph_solution_facts` for the whole envelope rather
// than just the pipeline.
//
// `graph_solution_facts` answers "what does this pipeline claim to do?" — the right question for
// an `action` turn and the wrong one for every other mode, because an `answer`, a `clarify` and
// an `abstain` carry no pipeline at all. Their correctness lives in the message and in what the
// model did NOT do, so grading them needs a projection over the response as a whole.
//
// Everything here is deterministic — no learned reward model and no LLM judge — for the same
// structural reason the correctness gate is deterministic: the ENGINE retrieved the context, so
// ground truth is known. That is what makes "did this answer invent a number?" a decidable
// question rather than an opinion, and it is the check that closes the fabrication failure mode
// prompting could never close.
namespace zetta::agent {

// The facts of a response, given the context the harness supplied when it asked.
//
// Always emitted:
//   kind                  — the declared mode, so a WRONG mode fails its task's grader outright
//   built_nothing         — no pipeline and no tool calls (what `clarify` and `abstain` require)
//   asked_one_question    — the message asks exactly one question (the `clarify` contract)
//   abstain / clarify     — set from the declared mode, so refusal families grade directly
//
// Emitted when the response was grounded in supplied context:
//   numbers_from_context  — EVERY number in the message also appears in the context. This is the
//                           anti-fabrication check: a fluent answer citing a plausible number
//                           that was never retrieved fails here.
//   grounded              — numbers_from_context, and the message actually said something
//   top_driver            — the highest-importance driver in the context, when the message names it
//   top_account           — the highest-risk account in the context, when the message names it
//   measure_auc           — the context's AUC, when the message cites that exact figure
//   table / row_count / node_count / running_jobs / run_status / failure_reason
//                         — likewise: emitted only when the message actually carries the value
//
// Plus every fact the pipeline carries (`graph_solution_facts`), so an `action` turn keeps
// grading exactly as it did before this existed.
// When `history` is supplied, two more facts about the turn's RELATIONSHIP to the one before it:
//
//   repeated_prior_turn   — the message is (modulo whitespace/case) the previous reply, verbatim
//   restated_prior_turn   — history exists, the message says something, and it is NOT the previous
//                           reply repeated
//
// These exist because "what?" returned a byte-identical reply to the turn before it, and nothing
// in the corpus or the grader could express that as a failure — the response was well-formed,
// grounded, and useless. Repetition is mechanically checkable against the history, so it is a real
// verifiable reward rather than a judgement call. Empty history emits neither fact, so every
// single-turn case grades exactly as it did before.
EndState response_solution_facts(const StudioAction& action, const std::string& grounding,
                                 const std::vector<PriorTurn>& history = {});

}  // namespace zetta::agent
