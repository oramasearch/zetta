#include "zetta/agent/agent_record.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

zetta::agent::AgentRecord sample() {
    zetta::agent::AgentRecord agent;
    agent.id = "a1b2";
    agent.name = "Base agent";
    agent.description = "The RLVR-trained base agent";
    agent.model = "data/checkpoints/base-agent-0.6b";
    agent.system_prompt = "You build data/AI pipelines.";
    agent.tools = {"connect", "clean", "model", "index", "serve", "act"};
    agent.created_at = "2026-07-22T00:00:00Z";
    agent.updated_at = "2026-07-22T00:00:00Z";
    return agent;
}

TEST(AgentRecord, RoundTripsThroughJson) {
    const zetta::agent::AgentRecord original = sample();
    std::string error;
    const auto parsed =
        zetta::agent::agent_from_json(zetta::agent::to_json(original), &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->id, original.id);
    EXPECT_EQ(parsed->name, original.name);
    EXPECT_EQ(parsed->model, original.model);
    EXPECT_EQ(parsed->system_prompt, original.system_prompt);
    EXPECT_EQ(parsed->tools, original.tools);
}

TEST(AgentRecord, RejectsAMissingName) {
    auto json = zetta::agent::to_json(sample());
    json.erase("name");
    std::string error;
    EXPECT_FALSE(zetta::agent::agent_from_json(json, &error).has_value());
    EXPECT_NE(error.find("name"), std::string::npos) << error;
}

TEST(AgentRecord, RejectsAMissingModel) {
    auto json = zetta::agent::to_json(sample());
    json["model"] = "";
    std::string error;
    EXPECT_FALSE(zetta::agent::agent_from_json(json, &error).has_value());
    EXPECT_NE(error.find("model"), std::string::npos) << error;
}

TEST(AgentRecord, RejectsANonObjectAndBadTools) {
    std::string error;
    EXPECT_FALSE(zetta::agent::agent_from_json(nlohmann::json::array(), &error).has_value());

    auto json = zetta::agent::to_json(sample());
    json["tools"] = "connect";  // not an array
    EXPECT_FALSE(zetta::agent::agent_from_json(json, &error).has_value());
    EXPECT_NE(error.find("tools"), std::string::npos) << error;
}

TEST(AgentRecord, RequiresAnId) {
    auto json = zetta::agent::to_json(sample());
    json["id"] = "";
    std::string error;
    EXPECT_FALSE(zetta::agent::agent_from_json(json, &error).has_value());
    EXPECT_NE(error.find("id"), std::string::npos) << error;
}

}  // namespace
