// SPDX-License-Identifier: MIT

#include "zetta/agent/chat.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

#include "checkpoint_registration.hpp"
#include "temp_dir.hpp"
#include "zetta/agent/checkpoint.hpp"
#include "zetta/agent/inference_runner.hpp"
#include "zetta/agent/model_cache.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/tokenizer/byte_encoding.hpp"

namespace {

using zetta::agent::ApplyStatus;
using zetta::agent::parse_tool_calls;
using zetta::agent::run_chat_turn;
using zetta::agent::Sandbox;
using zetta::agent::ToolCall;
using zetta::agent::ToolResult;

TEST(ParseToolCalls, ReadsAToolCallBlock) {
    const auto calls = parse_tool_calls(
        "Let me connect it.\n"
        "<tool_call>{\"name\": \"connect\", \"arguments\": {\"source\": \"warehouse\"}}</tool_call>");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].tool, "connect");
    EXPECT_EQ(calls[0].arguments.at("source"), "warehouse");
}

TEST(ParseToolCalls, ReadsMultipleBlocksAndSkipsGarbage) {
    const auto calls = parse_tool_calls(
        "<tool_call>{\"tool\": \"connect\"}</tool_call>"
        "<tool_call>not json</tool_call>"
        "<tool_call>{\"tool\": \"serve\", \"args\": {\"port\": 8080}}</tool_call>");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].tool, "connect");
    EXPECT_EQ(calls[1].tool, "serve");
    EXPECT_EQ(calls[1].arguments.at("port"), 8080);
}

TEST(ParseToolCalls, PlainTextHasNoCalls) {
    EXPECT_TRUE(parse_tool_calls("Here is your churn pipeline. Done.").empty());
}

TEST(ParseToolCalls, FallsBackToABareJsonObject) {
    const auto calls = parse_tool_calls("{\"tool\": \"index\", \"arguments\": {}}");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].tool, "index");
}

TEST(RunChatTurn, RunsAToolThenAnswers) {
    Sandbox sandbox;
    bool connected = false;
    sandbox.register_tool("connect", [&connected](zetta::agent::EndState&,
                                                  const nlohmann::json&) {
        connected = true;
        return ToolResult::success();
    });

    int calls = 0;
    const auto generate = [&calls](const std::string&) -> std::string {
        // First generation: a tool call. Second: the final answer.
        return ++calls == 1
                   ? "<tool_call>{\"tool\": \"connect\", \"arguments\": {}}</tool_call>"
                   : "Connected — your data is ready.";
    };

    const auto result = run_chat_turn(generate, sandbox, "connect my warehouse");
    EXPECT_TRUE(connected);
    EXPECT_EQ(result.reply, "Connected — your data is ready.");
    EXPECT_EQ(result.rounds, 2);
    ASSERT_EQ(result.invocations.size(), 1u);
    EXPECT_EQ(result.invocations[0].call.tool, "connect");
    EXPECT_EQ(result.invocations[0].status, ApplyStatus::Ok);
}

TEST(RunChatTurn, AnswersDirectlyWithNoTools) {
    Sandbox sandbox;
    const auto generate = [](const std::string&) -> std::string {
        return "The capital of France is Paris.";
    };
    const auto result = run_chat_turn(generate, sandbox, "capital of France?");
    EXPECT_EQ(result.reply, "The capital of France is Paris.");
    EXPECT_EQ(result.rounds, 1);
    EXPECT_TRUE(result.invocations.empty());
}

TEST(RunChatTurn, ReportsAnUnknownTool) {
    Sandbox sandbox;  // no tools registered
    int calls = 0;
    const auto generate = [&calls](const std::string&) -> std::string {
        return ++calls == 1 ? "<tool_call>{\"tool\": \"nope\"}</tool_call>"
                            : "Sorry, I could not do that.";
    };
    const auto result = run_chat_turn(generate, sandbox, "do a thing");
    ASSERT_EQ(result.invocations.size(), 1u);
    EXPECT_EQ(result.invocations[0].status, ApplyStatus::UnknownTool);
    EXPECT_EQ(result.reply, "Sorry, I could not do that.");
}

class AgentCheckpoints : public ::testing::Test {
protected:
    zetta::test::TempDir temporary_;
    zetta::test::CheckpointRegistration registration_;
    std::string directory_ = std::filesystem::canonical(temporary_.path()).string();
    zetta::agent::QwenInferenceRunner previous_runner_;

    void SetUp() override {
        registration_.configure("{}");
        if (zetta::agent::has_qwen_inference_runner()) {
            previous_runner_ = zetta::agent::qwen_inference_runner();
        }
        zetta::agent::install_qwen_inference_runner({});
        nlohmann::json vocabulary = nlohmann::json::object();
        for (int byte = 0; byte < 256; ++byte) {
            std::string symbol;
            zetta::tokenizer::utf8_encode(zetta::tokenizer::byte_to_unicode()[byte], symbol);
            vocabulary[symbol] = byte;
        }
        std::ofstream(temporary_.path() / "tokenizer.json")
            << nlohmann::json{{"model", {{"type", "BPE"}, {"vocab", vocabulary},
                                         {"merges", nlohmann::json::array()}}}}.dump();
        zetta::autograd::QwenConfig config;
        config.vocab_size = 256;
        std::string error;
        ASSERT_TRUE(zetta::autograd::save_qwen_model(
            zetta::autograd::QwenModel(config), directory_, &error)) << error;
    }
    void TearDown() override {
        zetta::agent::ModelCache::instance().unload_all();
        zetta::agent::install_qwen_inference_runner(previous_runner_);
    }
    zetta::agent::ChatResponse chat(const std::string& selector) {
        zetta::agent::AgentRecord agent;
        agent.model = selector;
        return zetta::agent::respond(agent, "hello", 1, 1);
    }
};

TEST_F(AgentCheckpoints, UnregisteredExistingCheckpointNeverReachesAnyLoader) {
    int generations = 0;
    zetta::agent::install_qwen_inference_runner(
        [&generations](const std::string&, const std::vector<int>&, int, int)
            -> std::optional<std::vector<int>> {
            ++generations;
            return std::vector<int>{'x'};
        });
    for (const auto& selector : {directory_, directory_ + "/.", directory_ + "/../" +
                                temporary_.path().filename().string(),
                                std::string("../outside"), std::string("%2fdata%2fmodels"),
                                std::string("base-agent\0suffix", 17)}) {
        SCOPED_TRACE(selector);
        const auto response = chat(selector);
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error, "agent.model: checkpoint is not registered");
        std::string error;
        EXPECT_FALSE(zetta::agent::ModelCache::instance().get(selector, &error));
        EXPECT_EQ(error, "agent.model: checkpoint is not registered");
        EXPECT_FALSE(zetta::agent::ModelCache::instance().load(selector, &error));
    }
    EXPECT_EQ(generations, 0);
}

TEST_F(AgentCheckpoints, ExactRegisteredSelectorsServeCpuAndGpuAndRevocationRejectsWarmCaches) {
    registration_.add("test-model", directory_);
    std::string error;
    const auto loaded = zetta::agent::ModelCache::instance().get("test-model", &error);
    ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(loaded->path, directory_);
    EXPECT_TRUE(chat("test-model").ok);
    int generations = 0;
    zetta::agent::install_qwen_inference_runner(
        [&](const std::string& directory, const std::vector<int>&, int, int)
            -> std::optional<std::vector<int>> {
            EXPECT_EQ(directory, directory_);
            ++generations;
            return std::vector<int>{'x'};
        });
    EXPECT_TRUE(chat("test-model").ok);
    EXPECT_EQ(generations, 1);
    // The same literal path works only when the operator explicitly registers it.
    registration_.add(directory_, directory_);
    EXPECT_TRUE(chat(directory_).ok);
    registration_.configure("{}");
    EXPECT_FALSE(chat("test-model").ok);
    EXPECT_FALSE(chat(directory_).ok);
    EXPECT_FALSE(zetta::agent::ModelCache::instance().get("test-model", &error));
    EXPECT_FALSE(zetta::agent::ModelCache::instance().get(directory_, &error));
    EXPECT_EQ(generations, 2);
}

TEST_F(AgentCheckpoints, SymlinkedDirectoriesAndFilesAreRefusedEvenWithWarmCaches) {
    registration_.add("test-model", directory_);
    ASSERT_TRUE(chat("test-model").ok);
    zetta::test::TempDir outside;
    const auto link = outside.path() / "linked";
    std::filesystem::create_directory_symlink(directory_, link);
    registration_.add("linked", std::filesystem::canonical(outside.path()).string() + "/linked");
    const auto linked = chat("linked");
    EXPECT_FALSE(linked.ok);
    EXPECT_NE(linked.error.find("without symlinks"), std::string::npos);
    registration_.add("linked-parent", link.string() + "/nested");
    std::filesystem::create_directory(temporary_.path() / "nested");
    EXPECT_FALSE(chat("linked-parent").ok);
    for (const auto* filename : {"tokenizer.json", "config.json", "model.safetensors"}) {
        SCOPED_TRACE(filename);
        const auto source = temporary_.path() / filename;
        const auto saved = outside.path() / filename;
        std::filesystem::rename(source, saved);
        std::filesystem::create_symlink(saved, source);
        const auto response = chat("test-model");
        EXPECT_FALSE(response.ok);
        EXPECT_NE(response.error.find("without symlinks"), std::string::npos);
        std::string error;
        EXPECT_FALSE(zetta::agent::ModelCache::instance().get("test-model", &error));
        std::filesystem::remove(source);
        std::filesystem::rename(saved, source);
    }
}

TEST_F(AgentCheckpoints, ShardReferencesCannotEscapeTheRegisteredDirectory) {
    registration_.add("test-model", directory_);
    const auto index = temporary_.path() / "model.safetensors.index.json";
    for (const auto& shard : {std::string("../outside.safetensors"),
                              directory_ + "/model.safetensors", std::string("nested/weights"),
                              std::string("nested\\weights"), std::string("."),
                              std::string("weights\0suffix", 14)}) {
        SCOPED_TRACE(shard);
        std::ofstream(index) << nlohmann::json{{"weight_map", {{"tensor", shard}}}}.dump();
        const auto response = chat("test-model");
        EXPECT_FALSE(response.ok);
        EXPECT_NE(response.error.find("checkpoint shard"), std::string::npos);
        std::string error;
        EXPECT_FALSE(zetta::agent::ModelCache::instance().get("test-model", &error));
    }
    std::ofstream(index) << nlohmann::json{{"weight_map", {{"tensor", "model.safetensors"}}}}.dump();
    EXPECT_TRUE(chat("test-model").ok);
}

TEST_F(AgentCheckpoints, InvalidOperatorConfigurationFailsClosedAndDefaultsRemainRegistered) {
    for (const auto* configuration : {"not json", "[]", "null", "{\"test-model\":3}",
                                      "{\"test-model\":\"relative\"}"}) {
        registration_.configure(configuration);
        EXPECT_FALSE(chat(directory_).ok);
        EXPECT_FALSE(chat("base-agent").ok);
    }
    unsetenv("ZETTA_AGENT_CHECKPOINTS");
    for (const auto* selector : {"base-agent", "/data/models/base-agent"}) {
        std::string error;
        const auto resolved = zetta::agent::checkpoint_directory(selector, &error);
        EXPECT_NE(error, "agent.model: checkpoint is not registered");
        if (resolved) EXPECT_EQ(*resolved, "/data/models/base-agent");
    }
}

}  // namespace
