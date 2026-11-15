// The conversation primitive: an ordered, multi-party thread that an agent's history is READ from
// rather than supplied. The properties under test are the ones that make multiplayer work and that
// a two-party model would have hidden.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "zetta/agent/conversation.hpp"

namespace {

using zetta::agent::Conversation;
using zetta::agent::Turn;
using zetta::agent::TurnAuthor;

Turn person(std::string id, std::string who, std::string text) {
    Turn turn;
    turn.id = std::move(id);
    turn.author = TurnAuthor::Person;
    turn.author_id = std::move(who);
    turn.text = std::move(text);
    return turn;
}

Turn agent(std::string id, std::string which, std::string text) {
    Turn turn;
    turn.id = std::move(id);
    turn.author = TurnAuthor::Agent;
    turn.author_id = std::move(which);
    turn.text = std::move(text);
    return turn;
}

TEST(Conversation, HistoryPairsAsksWithThisAgentsReplies) {
    Conversation thread;
    thread.id = "c1";
    thread.turns = {person("t1", "riley", "What can I build?"),
                    agent("t2", "a1", "Churn, lead scoring, or forecasting."),
                    person("t3", "riley", "List all of those options.")};

    const auto history = zetta::agent::history_for(thread, "a1");
    ASSERT_EQ(history.size(), 2U);
    EXPECT_EQ(history[0].ask, "What can I build?");
    EXPECT_EQ(history[0].reply, "Churn, lead scoring, or forecasting.");
    // The unanswered latest ask is still part of the exchange — dropping it would silently rewrite
    // what was said.
    EXPECT_EQ(history[1].ask, "List all of those options.");
    EXPECT_TRUE(history[1].reply.empty());
}

// THE multiplayer property. In a thread with two agents, each sees ITS OWN replies alongside what
// people said — attributing a colleague's answer to itself would teach it to imitate rather than
// answer, and to claim work it did not do.
TEST(Conversation, AnAgentsHistoryExcludesAnotherAgentsReplies) {
    Conversation thread;
    thread.id = "c2";
    thread.agents = {"analyst", "builder"};
    thread.turns = {person("t1", "riley", "What's driving churn?"),
                    agent("t2", "analyst", "Failed payments, mostly."),
                    agent("t3", "builder", "I can build a Pipeline for that."),
                    person("t4", "riley", "Do it.")};

    const auto for_builder = zetta::agent::history_for(thread, "builder");
    bool saw_analyst = false;
    for (const auto& turn : for_builder) {
        if (turn.reply == "Failed payments, mostly.") saw_analyst = true;
    }
    EXPECT_FALSE(saw_analyst) << "an agent must not be shown another agent's reply as its own";

    const auto for_analyst = zetta::agent::history_for(thread, "analyst");
    bool saw_builder = false;
    for (const auto& turn : for_analyst) {
        if (turn.reply == "I can build a Pipeline for that.") saw_builder = true;
    }
    EXPECT_FALSE(saw_builder);
}

// Several people in one thread is the same data — nothing assumes a single human.
TEST(Conversation, SeveralPeopleShareOneThread) {
    Conversation thread;
    thread.id = "c3";
    thread.participants = {"riley", "dana"};
    thread.turns = {person("t1", "riley", "Build churn."),
                    agent("t2", "a1", "Building it."),
                    person("t3", "dana", "Add calibration.")};

    const auto history = zetta::agent::history_for(thread, "a1");
    ASSERT_EQ(history.size(), 2U);
    EXPECT_EQ(history[0].ask, "Build churn.");
    EXPECT_EQ(history[1].ask, "Add calibration.") << "a second person's turn is still a turn";
}

// A tool result folds into the agent turn it belongs to — it is not something anyone SAID, it is
// what happened while the agent was answering. A follow-up asking "why did that fail?" needs it.
TEST(Conversation, ToolOutcomesRideWithTheAgentTurn) {
    Conversation thread;
    thread.id = "c4";
    Turn reply = agent("t2", "a1", "Started the run.");
    reply.tool_outcomes = {{"run", "failed", "the source connection timed out"}};
    thread.turns = {person("t1", "riley", "Run it."), reply,
                    person("t3", "riley", "Why did that fail?")};

    const auto history = zetta::agent::history_for(thread, "a1");
    ASSERT_GE(history.size(), 1U);
    ASSERT_EQ(history[0].tools.size(), 1U);
    EXPECT_EQ(history[0].tools[0].tool, "run");
    EXPECT_EQ(history[0].tools[0].status, "failed");
    EXPECT_NE(history[0].tools[0].detail.find("timed out"), std::string::npos)
        << "the reason lives in the outcome, not in the prose";
}

// A long thread will not fit a context window, and a follow-up refers to what was just said.
TEST(Conversation, LimitKeepsTheMostRecentExchanges) {
    Conversation thread;
    thread.id = "c5";
    for (int i = 0; i < 10; ++i) {
        thread.turns.push_back(person("p" + std::to_string(i), "riley", "ask " + std::to_string(i)));
        thread.turns.push_back(agent("a" + std::to_string(i), "a1", "reply " + std::to_string(i)));
    }
    const auto history = zetta::agent::history_for(thread, "a1", /*limit=*/3);
    ASSERT_EQ(history.size(), 3U);
    EXPECT_EQ(history.back().ask, "ask 9") << "the most recent exchanges, not the opening";
}

TEST(Conversation, RoundTripsThroughJson) {
    Conversation thread;
    thread.id = "c6";
    thread.title = "Churn work";
    thread.participants = {"riley"};
    thread.agents = {"a1"};
    Turn reply = agent("t2", "a1", "Building it.");
    reply.kind = "action";
    reply.grounded = true;
    reply.tool_calls.emplace_back("connect", nlohmann::json{{"name", "source"}});
    reply.tool_outcomes = {{"connect", "ok", ""}};
    reply.outcome = "accepted";
    thread.turns = {person("t1", "riley", "Build churn."), reply};

    std::string error;
    const auto restored =
        zetta::agent::conversation_from_json(zetta::agent::to_json(thread), &error);
    ASSERT_TRUE(restored.has_value()) << error;
    ASSERT_EQ(restored->turns.size(), 2U);
    EXPECT_EQ(restored->turns[1].kind, "action");
    EXPECT_TRUE(restored->turns[1].grounded);
    EXPECT_EQ(restored->turns[1].tool_calls.size(), 1U);
    EXPECT_EQ(restored->turns[1].tool_outcomes.size(), 1U);
    // The outcome is the reward signal no synthetic case can fake — it must survive.
    EXPECT_EQ(restored->turns[1].outcome, "accepted");
}

TEST(Conversation, AnUnattributedTurnIsRejected) {
    std::string error;
    EXPECT_FALSE(zetta::agent::turn_from_json({{"id", "t1"}, {"text", "hi"}}, &error).has_value());
    EXPECT_NE(error.find("author_id"), std::string::npos);
}

}  // namespace
