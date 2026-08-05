// The MCP prompts (#872): guided workflows over the tools. Pure JSON builders (no node), so
// this exercises them directly — the same shapes prompts/list and prompts/get return.
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/api/mcp_prompts.hpp"

namespace {

TEST(McpPrompts, ListsBuildAndRunWithItsArguments) {
    const nlohmann::json prompts = zetta::api::mcp_prompt_definitions();
    ASSERT_TRUE(prompts.is_array());
    const nlohmann::json* build_and_run = nullptr;
    for (const nlohmann::json& prompt : prompts) {
        if (prompt.value("name", "") == "build_and_run") build_and_run = &prompt;
    }
    ASSERT_NE(build_and_run, nullptr) << "build_and_run must be listed";
    EXPECT_FALSE(build_and_run->at("description").get<std::string>().empty());

    // ask is required; agent is optional — the shape a client renders as a form.
    bool ask_required = false, agent_optional = false;
    for (const nlohmann::json& argument : build_and_run->at("arguments")) {
        if (argument.at("name") == "ask") ask_required = argument.value("required", false);
        if (argument.at("name") == "agent") agent_optional = !argument.value("required", false);
    }
    EXPECT_TRUE(ask_required);
    EXPECT_TRUE(agent_optional);
}

TEST(McpPrompts, GetSubstitutesTheAskAndNamesTheTools) {
    std::string error;
    const std::optional<nlohmann::json> got = zetta::api::render_mcp_prompt(
        "build_and_run", {{"ask", "predict laptop returns"}}, &error);
    ASSERT_TRUE(got) << error;
    ASSERT_TRUE(got->at("messages").is_array());
    ASSERT_EQ(got->at("messages").size(), 1u);
    const std::string text = got->at("messages")[0].at("content").at("text").get<std::string>();
    EXPECT_NE(text.find("predict laptop returns"), std::string::npos);  // the ask is substituted
    EXPECT_NE(text.find("agent_run"), std::string::npos);               // it drives agent_run
    EXPECT_NE(text.find("get_run"), std::string::npos);                 // then polls get_run
}

TEST(McpPrompts, GetDefaultsTheAskWhenAbsent) {
    // No argument: the prompt still renders (a sensible churn default), so a bare
    // prompts/get is usable.
    const std::optional<nlohmann::json> got =
        zetta::api::render_mcp_prompt("build_and_run", nlohmann::json::object());
    ASSERT_TRUE(got);
    const std::string text = got->at("messages")[0].at("content").at("text").get<std::string>();
    EXPECT_NE(text.find("churn"), std::string::npos);
}

TEST(McpPrompts, GetRejectsUnknownPrompt) {
    std::string error;
    const std::optional<nlohmann::json> got =
        zetta::api::render_mcp_prompt("does_not_exist", nlohmann::json::object(), &error);
    EXPECT_FALSE(got);
    EXPECT_FALSE(error.empty());
}

}  // namespace
