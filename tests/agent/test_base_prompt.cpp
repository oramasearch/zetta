// The Orama Studio auto-AI agent's canonical system prompt + the shared train/inference framing
// (#873). One source of truth (base_prompt.cpp) is used by the SFT render and by agent::respond,
// so training and inference cannot drift.
#include <gtest/gtest.h>

#include <string>

#include "zetta/agent/base_prompt.hpp"

namespace {

TEST(BaseAgentPrompt, IsTheStudioAutoAiAgent) {
    const std::string prompt(zetta::agent::base_agent_system_prompt());
    EXPECT_FALSE(prompt.empty());
    // Identity: the Orama Studio auto-AI agent on Zetta.
    EXPECT_NE(prompt.find("Orama Studio"), std::string::npos);
    EXPECT_NE(prompt.find("pipeline"), std::string::npos);
    // The action contract it must emit.
    EXPECT_NE(prompt.find("message"), std::string::npos);
    EXPECT_NE(prompt.find("tool_calls"), std::string::npos);
    EXPECT_NE(prompt.find("execution_graph"), std::string::npos);
    // The step vocabulary — but explicitly NOT a fixed shape.
    for (const char* stage : {"connect", "clean", "model", "index", "serve", "act"}) {
        EXPECT_NE(prompt.find(stage), std::string::npos) << "missing step: " << stage;
    }
    // The production-grade disciplines the gate enforces (per use case).
    for (const char* discipline : {"leakage-free", "point-in-time", "holdout", "calibrat",
                                   "SHAP", "approval-gated"}) {
        EXPECT_NE(prompt.find(discipline), std::string::npos) << "missing: " << discipline;
    }
    // The families it recognizes, incl. abstain.
    EXPECT_NE(prompt.find("Search"), std::string::npos);
    EXPECT_NE(prompt.find("build nothing"), std::string::npos);  // out-of-scope abstain
    // The UI vocabulary law: never expose the jargon in the message.
    EXPECT_NE(prompt.find("never \"execution graph\""), std::string::npos);
}

TEST(BaseAgentPrompt, FramesTheTurnAsTrained) {
    const std::string framed = zetta::agent::agent_prompt_format(
        zetta::agent::base_agent_system_prompt(), "make our 50k support docs searchable");
    const std::size_t system_at = framed.find("Orama Studio");
    const std::size_t ask_at = framed.find("make our 50k support docs searchable");
    ASSERT_NE(system_at, std::string::npos);
    ASSERT_NE(ask_at, std::string::npos);
    EXPECT_LT(system_at, ask_at);  // system prompt precedes the ask; the model completes after
    EXPECT_TRUE(framed.ends_with("\n\n"));
}

TEST(BaseAgentPrompt, OmitsAnEmptySystemPrompt) {
    const std::string framed = zetta::agent::agent_prompt_format("", "index our FAQ");
    EXPECT_TRUE(framed.starts_with("index our FAQ"));
}

}  // namespace
