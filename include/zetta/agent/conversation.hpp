#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/interaction.hpp"
#include "zetta/agent/tool_call.hpp"

// A CONVERSATION — an ordered, replicated thread of turns between people and agents.
//
// Why a first-class entity rather than a parameter. Chat carried its history in the REQUEST, which
// made three things impossible at once: a follow-up only worked if the caller remembered to send
// the thread back (the web console did not, so every turn was a first turn); two people could not
// be in the same exchange, because there was no shared thing to be in; and nothing was recorded, so
// the most valuable training data this product generates — real asks, real follow-ups, and what the
// user did next — was discarded at the end of every request.
//
// One entity fixes all three. The thread lives on the node, replicated like every other entity, so
// any participant on any node sees the same exchange, an agent's history is READ from it rather
// than supplied, and the turns are already the corpus (#943).
//
// **Multiplayer by construction.** A turn carries its author, and an author is a principal or an
// agent. Nothing in the model assumes one human and one agent — two people and two agents in one
// thread is the same data. That is deliberate: retrofitting multi-party onto a two-party model
// means reworking every consumer, and the shape costs nothing now.
//
// #942 (multi-turn) · #943 (turns as the training corpus) · #849 (agents) · docs/agent/README.md.
namespace zetta::agent {

// Who said a thing. A turn is authored by a PRINCIPAL (a person or a key) or by an AGENT, and the
// distinction is load-bearing: an agent's own turns become its history, a person's turns are what
// it is answering, and a tool result is neither.
enum class TurnAuthor {
    Person,  // a principal — `author` is the principal id
    Agent,   // an agent — `author` is the agent id
    Tool,    // a tool result fed back into the thread; `author` is the tool name
};

std::string_view to_string(TurnAuthor author);
std::optional<TurnAuthor> turn_author_from_string(std::string_view name);

// One turn. Everything needed to reconstruct the exchange AND to grade it later as a training case.
struct Turn {
    std::string id;                 // uuid, stable across replicas
    TurnAuthor author = TurnAuthor::Person;
    std::string author_id;          // principal id / agent id / tool name
    std::string text;               // what was said — for an agent, its MESSAGE, never the envelope
    std::string created_at;         // ISO-8601, supplied by the caller (no clock in the core)

    // Agent turns only — what it decided and what actually ran. Kept because a follow-up refers to
    // it ("why did that fail?") and because a graded training case needs it.
    std::string kind;                       // "action" / "answer" / "clarify" / "abstain"
    std::vector<ToolCall> tool_calls;
    std::vector<ToolOutcome> tool_outcomes;  // what each call returned
    std::string grounding;                   // the context retrieved for this turn, if any
    bool grounded = false;                   // did it answer FROM that context?

    // What the person did with it, set later by the UI: "accepted" / "edited" / "rephrased" /
    // "abandoned". Empty until known. This is the reward signal no synthetic case can fake, and it
    // is why it is a mutable field rather than something captured at generation time.
    std::string outcome;

    bool operator==(const Turn&) const = default;
};

struct Conversation {
    std::string id;
    std::string title;                        // human label; may be derived from the first ask
    std::vector<std::string> participants;    // principal ids that have posted or been invited
    std::vector<std::string> agents;          // agent ids taking part
    std::vector<Turn> turns;                  // oldest first
    std::string created_at;
    std::string updated_at;

    // Empty when well-formed enough to serve; else the reason.
    std::string validation_error() const;
};

// The history an agent is given for its next turn, derived from the thread.
//
// Person and agent turns pair into exchanges; a tool turn folds into the agent turn it belongs to.
// `for_agent` matters in a multi-agent thread: an agent's history is what IT said, alongside what
// people said — attributing another agent's replies to itself would teach it to imitate a
// colleague rather than answer.
//
// `limit` bounds how far back to look (0 = the whole thread), because a long thread will not fit a
// context window and the recent turns are the ones a follow-up refers to.
std::vector<PriorTurn> history_for(const Conversation& conversation, const std::string& for_agent,
                                   std::size_t limit = 0);

nlohmann::json to_json(const Turn& turn);
nlohmann::json to_json(const Conversation& conversation);
std::optional<Turn> turn_from_json(const nlohmann::json& value, std::string* error = nullptr);
std::optional<Conversation> conversation_from_json(const nlohmann::json& value,
                                                   std::string* error = nullptr);
const nlohmann::json& conversation_schema();

}  // namespace zetta::agent
