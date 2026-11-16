// SPDX-License-Identifier: MIT

// The #969 loop end to end: chat agent → proposal → approval → run → report → chat
// introspection, through the real control surface.
//
//   * Layer 1 (in-process registry) and layer 2 (real JSON-RPC over loopback HTTP)
//     replay the SAME journey tables (journeys_canonical.hpp: chat_loop_happy_path,
//     chat_loop_refusals), per docs/testing/mcp-journeys.md.
//   * The chat legs run a real `chat` turn with a SCRIPTED model: a byte-level tokenizer
//     on disk plus an installed inference runner that answers from a script. Everything
//     between the model and the node is the production path (prompt, envelope parse,
//     tool sandbox, grounding, the proposal); only the weights are replaced, so the test
//     is deterministic and needs no checkpoint.
//   * #975: POST /v1/chat answers with the same payload MCP does.
//
// Replay locally:  ./build/tests/zetta-tests --gtest_filter='ChatLoop*'
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "journey.hpp"
#include "journeys_canonical.hpp"
#include "temp_dir.hpp"
#include "zetta/agent/chat.hpp"
#include "zetta/agent/inference_runner.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/server.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/run_proposal.hpp"
#include "zetta/tokenizer/bpe.hpp"
#include "zetta/tokenizer/byte_encoding.hpp"
#include "agent/checkpoint_registration.hpp"

namespace {

namespace journey = zetta::test::journey;
using Json = nlohmann::json;
using zetta::api::ApiContext;
using zetta::api::ApiResult;

constexpr std::int64_t kAccounts = 400;

// 400 accounts whose churn follows tickets, failed payments and short tenure, with every
// 17th label flipped so the problem is not separable (the Getting started shape, #1216).
std::string accounts_csv() {
    std::string csv = "account_id,support_tickets,tenure_months,monthly_spend,failed_payments,churned\n";
    for (int index = 0; index < kAccounts; ++index) {
        const int support_tickets = (index * 7) % 11;
        const int tenure_months = 1 + (index * 13) % 72;
        const double monthly_spend = 19.0 + ((index * 37) % 400) * 0.75 + (index % 7) * 0.01;
        const int failed_payments = (index * 5) % 4;
        const double risk = 0.3 * support_tickets + 0.8 * failed_payments - 0.04 * tenure_months;
        int churned = risk > 1.5 ? 1 : 0;
        if (index % 17 == 0) churned = 1 - churned;
        csv += std::format("{},{},{},{:.2f},{},{}\n", 5000 + index, support_tickets,
                           tenure_months, monthly_spend, failed_payments, churned);
    }
    return csv;
}

// ---- the scripted model ---------------------------------------------------------------
//
// A checkpoint directory holding only a byte-level tokenizer (one token per byte, plus the
// Qwen turn markers), and an inference runner that decodes the prompt, asks `script` for
// the completion, and encodes it back. `prompts` records every prompt the model saw, which
// is how a test proves a tool's RESULT reached the model.
class ScriptedModel {
public:
    using Script = std::function<std::string(const std::string& prompt)>;

    ScriptedModel() {
        registration_.add(path(), path());
        Json vocab = Json::object();
        for (int byte = 0; byte < 256; ++byte) {
            std::string symbol;
            zetta::tokenizer::utf8_encode(zetta::tokenizer::byte_to_unicode()[byte], symbol);
            vocab[symbol] = byte;
        }
        const Json tokenizer = {
            {"added_tokens",
             Json::array({{{"id", 256}, {"content", "<|im_end|>"}, {"special", true}},
                          {{"id", 257}, {"content", "<|im_start|>"}, {"special", true}},
                          {{"id", 258}, {"content", "<|endoftext|>"}, {"special", true}}})},
            {"model", {{"type", "BPE"}, {"vocab", vocab}, {"merges", Json::array()}}}};
        std::ofstream(directory_.path() / "tokenizer.json") << tokenizer.dump();
        std::string error;
        auto loaded = zetta::tokenizer::BpeTokenizer::from_directory(path(), &error);
        EXPECT_TRUE(loaded.has_value()) << error;
        tokenizer_ = std::make_shared<zetta::tokenizer::BpeTokenizer>(std::move(*loaded));

        if (zetta::agent::has_qwen_inference_runner()) {
            previous_ = zetta::agent::qwen_inference_runner();
        }
        zetta::agent::install_qwen_inference_runner(
            [this](const std::string& model_path, const std::vector<int>& prompt_tokens, int,
                   int) -> std::optional<std::vector<int>> {
                if (model_path != path()) return std::nullopt;
                const std::string prompt = tokenizer_->decode(prompt_tokens);
                std::string completion;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    prompts_.push_back(prompt);
                    completion = script_ ? script_(prompt) : std::string();
                }
                return tokenizer_->encode(completion);
            });
    }
    ~ScriptedModel() { zetta::agent::install_qwen_inference_runner(previous_); }

    std::string path() const { return std::filesystem::canonical(directory_.path()).string(); }
    void script(Script next) {
        std::lock_guard<std::mutex> lock(mutex_);
        script_ = std::move(next);
        prompts_.clear();
    }
    std::vector<std::string> prompts() {
        std::lock_guard<std::mutex> lock(mutex_);
        return prompts_;
    }

private:
    zetta::test::CheckpointRegistration registration_;
    zetta::test::TempDir directory_;
    std::shared_ptr<zetta::tokenizer::BpeTokenizer> tokenizer_;
    zetta::agent::QwenInferenceRunner previous_;
    std::mutex mutex_;
    Script script_;
    std::vector<std::string> prompts_;
};

// What the trained agent emits for "build me this": an action envelope whose tool calls
// build the Pipeline (the base agent's card, docs/agent/cards/models/base-agent.md).
std::string action_envelope() {
    return Json{{"kind", "action"},
                {"message", "I'll connect your accounts, build churn features, train a model "
                            "and rank the accounts most at risk."},
                {"tool_calls", Json::array({{{"tool", "connect"}},
                                            {{"tool", "clean"}},
                                            {{"tool", "model"}},
                                            {{"tool", "serve"}}})}}
        .dump();
}

std::string answer_envelope(const std::string& message) {
    return Json{{"kind", "answer"}, {"message", message}}.dump();
}

// ---- a minimal HTTP/1.1 client ---------------------------------------------------------

struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http_request(unsigned short port, const std::string& method,
                          const std::string& target, const std::string& body,
                          const std::string& bearer) {
    HttpResponse response;
    const int socket_descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0) return response;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(socket_descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_descriptor);
        return response;
    }
    std::string request = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!body.empty()) request += "Content-Type: application/json\r\n";
    request += "Authorization: Bearer " + bearer + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n" + body;
    ::send(socket_descriptor, request.data(), request.size(), 0);
    std::string raw;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = ::recv(socket_descriptor, buffer, sizeof(buffer), 0)) > 0) {
        raw.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(socket_descriptor);
    if (raw.size() > 12 && raw.starts_with("HTTP/1.1 ")) response.status = std::stoi(raw.substr(9, 3));
    if (const auto separator = raw.find("\r\n\r\n"); separator != std::string::npos) {
        response.body = raw.substr(separator + 4);
        // A chunked body: keep the single JSON document between the chunk framing.
        if (raw.find("Transfer-Encoding: chunked") != std::string::npos ||
            raw.find("transfer-encoding: chunked") != std::string::npos) {
            const auto open = response.body.find('{');
            const auto close = response.body.rfind('}');
            if (open != std::string::npos && close != std::string::npos && open < close) {
                response.body = response.body.substr(open, close - open + 1);
            }
        }
    }
    return response;
}

std::string query_encode(std::string_view text) {
    std::string out;
    for (const unsigned char character : text) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            out += static_cast<char>(character);
        } else {
            out += std::format("%{:02X}", character);
        }
    }
    return out;
}

// ---- the fixture -----------------------------------------------------------------------

// A replicated node (proposals and agents are replicated state) with its compute plane
// attached (a single-member JobCoordinator, so runs execute), the in-process registry for
// layer 1, and the embedded server for layer 2 and REST.
class ChatLoop : public ::testing::Test {
protected:
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{"chat-loop-node"};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}, &replication};
    zetta::cluster::Cluster cluster{"chat-loop-node", [] {
                                        zetta::cluster::ClusterConfig config;
                                        config.listen_host = "127.0.0.1";
                                        config.listen_port = 24960;
                                        config.port_range = 16;
                                        return config;
                                    }()};
    zetta::api::Registry registry;
    zetta::api::BackgroundActions background;
    std::unique_ptr<zetta::api::ApiServer> server;
    std::string owner_secret = "chat-loop-owner-secret";

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
        cluster.start();
        node.attach_cluster(cluster, /*worker_count=*/2);
        ASSERT_NE(node.job_coordinator(), nullptr);
        zetta::api::register_core_capabilities(registry);
        zetta::api::ServerOptions options;
        options.port = 0;
        server = std::make_unique<zetta::api::ApiServer>(node, &cluster, options);
        ASSERT_TRUE(server->start()) << server->reason();
        // Adopt this thread as the owner loop, as the apps do.
        node.tick();
    }
    void TearDown() override {
        server->stop();
        background.prune();
        node.stop();
        cluster.stop();
    }

    // Layer 1: the registry the MCP endpoint renders, called in process.
    ApiResult call(const std::string& name, const Json& input) {
        ApiContext context{node, &cluster, {}, {}, &node.span_recorder(), &background};
        return registry.call(name, input, context);
    }
    journey::Caller layer1() {
        return [this](const std::string& tool, const Json& input) { return call(tool, input); };
    }
    journey::Pump layer1_pump() {
        return [this] {
            node.tick();
            background.prune();
        };
    }

    // An HTTP request from a helper thread while this (owning) thread drains the server.
    HttpResponse request(const std::string& method, const std::string& target,
                         const std::string& body = {}) {
        HttpResponse response;
        std::atomic<bool> finished{false};
        std::thread client([&] {
            response = http_request(server->port(), method, target, body, owner_secret);
            finished = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            server->drain();
            node.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        client.join();
        return response;
    }

    // Layer 2: each step is a real tools/call over the wire, mapped back to an ApiResult.
    journey::Caller layer2() {
        return [this](const std::string& tool, const Json& input) {
            static std::atomic<int> next_id{1};
            const Json payload = {{"jsonrpc", "2.0"},
                                  {"id", next_id++},
                                  {"method", "tools/call"},
                                  {"params", {{"name", tool}, {"arguments", input}}}};
            const HttpResponse response = request("POST", "/mcp", payload.dump());
            const Json parsed = Json::parse(response.body, nullptr, /*allow_exceptions=*/false);
            ApiResult result;
            if (!parsed.is_object() || !parsed.contains("result")) {
                result.error = {"protocol_error", response.body, ""};
                return result;
            }
            const Json& structured = parsed.at("result").at("structuredContent");
            result.ok = structured.value("ok", false);
            if (result.ok) {
                result.value = structured.value("value", Json::object());
            } else {
                const Json error = structured.value("error", Json::object());
                result.error = {error.value("code", std::string{}), error.value("message", std::string{}),
                                error.value("path", std::string{})};
            }
            return result;
        };
    }
    journey::Pump layer2_pump() {
        return [this] {
            node.tick();
            server->drain();
        };
    }

    // Pump until the run is terminal; the status it ended in.
    zetta::pipeline::RunStatus finish_run(const std::string& run_id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (std::chrono::steady_clock::now() < deadline) {
            node.tick();
            background.prune();
            server->drain();
            if (const auto run = node.find_run(run_id);
                run && (run->status() == zetta::pipeline::RunStatus::Succeeded ||
                        run->status() == zetta::pipeline::RunStatus::Failed)) {
                return run->status();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return zetta::pipeline::RunStatus::Running;
    }

    std::string create_agent(const std::string& model, const Json& tools) {
        const ApiResult created =
            call("create_agent", {{"name", "Ask Orama"}, {"model", model}, {"tools", tools}});
        EXPECT_TRUE(created.ok) << created.error.message;
        return created.value.value("id", std::string{});
    }
};

// ---- layer 1 and layer 2: the journey tables -------------------------------------------

TEST_F(ChatLoop, JourneyProposeApproveReportExplainInProcess) {
    const journey::JourneyReport report =
        journey::run_journey(journey::canonical::chat_loop_happy_path(accounts_csv(), kAccounts),
                             layer1(), layer1_pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(ChatLoop, JourneyRefusalsInProcess) {
    const auto bare = node.create_run("chat loop bare run");
    ASSERT_TRUE(bare);
    const journey::JourneyReport report = journey::run_journey(
        journey::canonical::chat_loop_refusals(bare->id()), layer1(), layer1_pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(ChatLoop, JourneyProposeApproveReportExplainOverMcp) {
    const journey::JourneyReport report =
        journey::run_journey(journey::canonical::chat_loop_happy_path(accounts_csv(), kAccounts),
                             layer2(), layer2_pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(ChatLoop, JourneyRefusalsOverMcp) {
    const auto bare = node.create_run("chat loop bare run");
    ASSERT_TRUE(bare);
    const journey::JourneyReport report = journey::run_journey(
        journey::canonical::chat_loop_refusals(bare->id()), layer2(), layer2_pump());
    EXPECT_TRUE(report.ok) << report.summary();
}

// The approval challenge an MCP client renders: the open proposal names the exact call
// that approves it and the one that rejects it, and the decided one names neither.
TEST_F(ChatLoop, TheApprovalChallengeOverMcpNamesBothDecisions) {
    const auto caller = layer2();
    const ApiResult proposed = caller("propose_run", {{"message", "Predict which customers will churn"}});
    ASSERT_TRUE(proposed.ok) << proposed.error.message;
    const std::string id = proposed.value.at("proposal_id");
    EXPECT_EQ(proposed.value.at("decide").at("approve"),
              (Json{{"tool", "approve_run_proposal"}, {"input", {{"proposal_id", id}}}}));
    EXPECT_EQ(proposed.value.at("decide").at("reject"),
              (Json{{"tool", "reject_run_proposal"}, {"input", {{"proposal_id", id}}}}));
    const ApiResult rejected = caller("reject_run_proposal", {{"proposal_id", id}});
    ASSERT_TRUE(rejected.ok) << rejected.error.message;
    EXPECT_FALSE(rejected.value.contains("decide")) << rejected.value.dump();
    // Rejecting twice changes nothing.
    const ApiResult again = caller("reject_run_proposal", {{"proposal_id", id}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value.at("decided_at"), rejected.value.at("decided_at"));
}

// A caller whose key may only read cannot propose, and so cannot get a run started by
// asking: chat's proposal goes through the registry under the caller's own rights.
TEST_F(ChatLoop, ProposingNeedsItsOwnRight) {
    auto principal = node.create_principal("reader", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    zetta::governance::CapabilitySet grants;
    grants.grant(zetta::governance::CapabilityDomain::Api, "query");
    auto key = principal->create_key(zetta::governance::unrestricted_scope(), std::move(grants));
    ASSERT_TRUE(key);
    ApiContext context{node, &cluster, key->id(), {}, &node.span_recorder(), &background};
    const ApiResult refused =
        registry.call("propose_run", {{"message", "Predict which customers will churn"}}, context);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "unauthorized") << refused.error.message;
    EXPECT_TRUE(zetta::pipeline::RunProposalStore(replication).list().empty());
}

// ---- the chat legs, with a scripted model ----------------------------------------------

// Leg 1 (chat agent → project), the half that was missing: an `action` turn PROPOSES the
// run and creates nothing; approving creates exactly what the preview named and runs it.
// Then the follow-up, asked without naming the run, grounds on it and the agent's own
// get_run_report call returns the report to the model (leg 4, chat introspect).
TEST_F(ChatLoop, ChatProposesTheRunThenAFollowUpReadsItsReport) {
    ScriptedModel model;
    ASSERT_TRUE(call("upload_dataset", {{"database", "chat_loop"},
                                        {"table_name", "accounts"},
                                        {"format", "csv"},
                                        {"content", accounts_csv()}})
                    .ok);
    // An agent whose record names only read tools: building must still work (#969).
    const std::string agent = create_agent(model.path(), {"get_run_report", "explain", "list_projects"});

    model.script([](const std::string&) { return action_envelope(); });
    const ApiResult turn = call("chat", {{"agent", agent},
                                         {"message", "Set up a churn project on my accounts and "
                                                     "tell me which accounts are most at risk."},
                                         {"dataset_path", "chat_loop.accounts"},
                                         {"holdout_fraction", 0.25}});
    ASSERT_TRUE(turn.ok) << turn.error.message;
    EXPECT_EQ(turn.value.at("kind"), "action") << turn.value.dump();
    EXPECT_EQ(turn.value.value("pipeline_steps", 0), 4) << turn.value.dump();
    for (const Json& tool_call : turn.value.at("tool_calls")) {
        EXPECT_EQ(tool_call.at("status"), "ok") << "a built step reported as failed: " << tool_call.dump();
    }
    ASSERT_TRUE(turn.value.contains("proposal")) << turn.value.dump();
    const Json& proposal = turn.value.at("proposal");
    EXPECT_EQ(proposal.at("approval"), "proposed");
    EXPECT_EQ(proposal.at("agent"), agent);
    EXPECT_EQ(proposal.at("preview").at("dataset").at("path"), "chat_loop.accounts");
    const std::string project = proposal.at("preview").at("project");

    // The negative read-back: the proposal created nothing.
    EXPECT_TRUE(call("list_projects", Json::object()).value.at("projects").empty());
    EXPECT_TRUE(node.runs().empty());

    const ApiResult approved =
        call("approve_run_proposal", {{"proposal_id", proposal.at("proposal_id")}});
    ASSERT_TRUE(approved.ok) << approved.error.message;
    EXPECT_EQ(approved.value.at("project"), project);
    const std::string run_id = approved.value.at("run").at("run_id");
    ASSERT_EQ(finish_run(run_id), zetta::pipeline::RunStatus::Succeeded);
    const ApiResult report = call("get_run_report", {{"run_id", run_id}, {"limit", 3}});
    ASSERT_TRUE(report.ok) << report.error.message;
    const std::string top_entity = report.value.at("at_risk").at(0).at("entity").dump();

    // The follow-up names no run and no project. The model calls get_run_report, then
    // answers from what came back.
    int round = 0;
    model.script([&](const std::string&) {
        if (round++ == 0) {
            return "<tool_call>" +
                   Json{{"tool", "get_run_report"}, {"arguments", {{"run_id", run_id}, {"limit", 3}}}}
                       .dump() +
                   "</tool_call>";
        }
        return answer_envelope("Account " + top_entity + " is the most at risk.");
    });
    const ApiResult follow_up =
        call("chat", {{"agent", agent}, {"message", "Which accounts are most at risk?"}});
    ASSERT_TRUE(follow_up.ok) << follow_up.error.message;
    EXPECT_EQ(follow_up.value.at("kind"), "answer") << follow_up.value.dump();
    EXPECT_EQ(follow_up.value.value("grounded_project", std::string{}), project);
    EXPECT_TRUE(follow_up.value.at("grounded").get<bool>());
    EXPECT_FALSE(follow_up.value.contains("proposal")) << "an answer proposes nothing";
    ASSERT_EQ(follow_up.value.at("tool_calls").size(), 1u) << follow_up.value.dump();
    EXPECT_EQ(follow_up.value.at("tool_calls").at(0).at("tool"), "get_run_report");
    EXPECT_EQ(follow_up.value.at("tool_calls").at(0).at("status"), "ok");
    EXPECT_NE(follow_up.value.at("reply").get<std::string>().find(top_entity), std::string::npos);

    // The model's second round SAW the report: its at-risk list, with the top account.
    const std::vector<std::string> prompts = model.prompts();
    ASSERT_EQ(prompts.size(), 2u);
    // (The tool instructions mention the tag too; the fed-back result is the tag + JSON.)
    const std::size_t result = prompts[1].find("<tool_result>{");
    ASSERT_NE(result, std::string::npos) << prompts[1];
    const std::string fed_back = prompts[1].substr(result);
    EXPECT_NE(fed_back.find("\"at_risk\""), std::string::npos) << fed_back;
    EXPECT_NE(fed_back.find("\"entity\":" + top_entity), std::string::npos) << fed_back;
    // …and the grounding named the run's Project before the model said anything.
    EXPECT_NE(prompts[0].find("Project: " + project), std::string::npos) << prompts[0];
}

// A caller who may not propose runs still gets the reply, with the reason no proposal was
// recorded, and nothing is created.
TEST_F(ChatLoop, ChatWithoutTheRightToProposeSaysSoAndCreatesNothing) {
    ScriptedModel model;
    const std::string agent = create_agent(model.path(), {"get_run_report"});
    model.script([](const std::string&) { return action_envelope(); });
    auto principal = node.create_principal("chatter", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    zetta::governance::CapabilitySet grants;
    grants.grant(zetta::governance::CapabilityDomain::Api, "query");
    auto key = principal->create_key(zetta::governance::unrestricted_scope(), std::move(grants));
    ASSERT_TRUE(key);
    ApiContext context{node, &cluster, key->id(), {}, &node.span_recorder(), &background};
    const ApiResult turn = registry.call(
        "chat", {{"agent", agent}, {"message", "Predict which customers will churn"}}, context);
    ASSERT_TRUE(turn.ok) << turn.error.message;
    EXPECT_EQ(turn.value.at("kind"), "action");
    EXPECT_FALSE(turn.value.contains("proposal"));
    EXPECT_NE(turn.value.value("proposal_warning", std::string{}).find("none was recorded"),
              std::string::npos)
        << turn.value.dump();
    EXPECT_TRUE(zetta::pipeline::RunProposalStore(replication).list().empty());
    EXPECT_TRUE(node.project_names().empty());
}

// ---- #975: POST /v1/chat answers, with the payload MCP returns -------------------------

TEST_F(ChatLoop, PostV1ChatAnswersWithTheSamePayloadAsMcp) {
    ScriptedModel model;
    const std::string agent = create_agent(model.path(), Json::array());
    model.script([](const std::string&) { return answer_envelope("Hello. Ask me about your data."); });

    const Json input = {{"agent", agent}, {"message", "hello"}};
    const HttpResponse posted = request("POST", "/v1/chat", input.dump());
    ASSERT_EQ(posted.status, 200) << posted.body;
    const Json rest = Json::parse(posted.body, nullptr, false);
    ASSERT_TRUE(rest.is_object()) << posted.body;
    ASSERT_TRUE(rest.at("ok").get<bool>()) << posted.body;

    const ApiResult mcp = layer2()("chat", input);
    ASSERT_TRUE(mcp.ok) << mcp.error.message;
    EXPECT_EQ(rest.at("value"), mcp.value);
    EXPECT_EQ(rest.at("value").at("reply"), "Hello. Ask me about your data.");
    EXPECT_EQ(rest.at("value").at("kind"), "answer");

    // GET still serves it, with the input in the query string.
    const HttpResponse got = request(
        "GET", "/v1/chat?agent=" + query_encode(agent) + "&message=" + query_encode("hello"));
    ASSERT_EQ(got.status, 200) << got.body;
    EXPECT_EQ(Json::parse(got.body, nullptr, false).at("value"), mcp.value);

    // A refusal crosses both renderings identically too (the error envelope, not silence).
    const Json unknown = {{"agent", "no-such-agent"}, {"message", "hello"}};
    const HttpResponse refused = request("POST", "/v1/chat", unknown.dump());
    EXPECT_EQ(refused.status, 404) << refused.body;
    const ApiResult refused_mcp = layer2()("chat", unknown);
    EXPECT_EQ(Json::parse(refused.body, nullptr, false).at("error").at("message"),
              refused_mcp.error.message);
}

// The proposal crosses REST as well: an `action` turn over POST /v1/chat carries it, and
// POST /v1/approve_run_proposal starts the run.
TEST_F(ChatLoop, PostV1ChatProposesAndRestApprovesTheRun) {
    ScriptedModel model;
    const std::string agent = create_agent(model.path(), Json::array());
    model.script([](const std::string&) { return action_envelope(); });
    const HttpResponse posted = request(
        "POST", "/v1/chat",
        Json{{"agent", agent}, {"message", "Predict which customers will churn"}}.dump());
    ASSERT_EQ(posted.status, 200) << posted.body;
    const Json turn = Json::parse(posted.body, nullptr, false).at("value");
    ASSERT_TRUE(turn.contains("proposal")) << turn.dump();
    EXPECT_TRUE(node.project_names().empty());

    const HttpResponse approved = request(
        "POST", "/v1/approve_run_proposal",
        Json{{"proposal_id", turn.at("proposal").at("proposal_id")}}.dump());
    ASSERT_EQ(approved.status, 200) << approved.body;
    const Json value = Json::parse(approved.body, nullptr, false).at("value");
    EXPECT_EQ(value.at("project"), turn.at("proposal").at("preview").at("project"));
    EXPECT_EQ(finish_run(value.at("run").at("run_id")), zetta::pipeline::RunStatus::Succeeded);
}

// ---- the sandbox: live tools reach the runner (the b5ca4290 regression) ----------------

TEST(ChatSandbox, LiveToolsReachTheRunnerAndStepsAreNeverUnknown) {
    zetta::agent::AgentRecord agent;
    agent.id = "agent";
    agent.name = "Ask Orama";
    agent.model = "/nowhere";
    agent.tools = {"get_run_report"};
    std::vector<std::pair<std::string, Json>> calls;
    const zetta::agent::AgentToolRunner runner = [&calls](const std::string& tool,
                                                          const Json& arguments) {
        calls.emplace_back(tool, arguments);
        return zetta::agent::ToolResult::success(Json{{"at_risk", Json::array({{{"entity", 7}}})}});
    };
    zetta::agent::Sandbox sandbox = zetta::agent::chat_sandbox(
        agent, runner, {{"get_run_report", "read what a finished run found"}});

    const zetta::agent::ApplyOutcome read =
        sandbox.apply({"get_run_report", Json{{"run_id", "r-1"}}});
    EXPECT_EQ(read.status, zetta::agent::ApplyStatus::Ok);
    ASSERT_EQ(calls.size(), 1u) << "the live tool never reached the runner";
    EXPECT_EQ(calls[0].second, (Json{{"run_id", "r-1"}}));
    EXPECT_EQ(read.data, (Json{{"at_risk", Json::array({{{"entity", 7}}})}}));

    // Every pipeline step kind is a recorded step, even though the record names none.
    for (const char* step : {"connect", "clean", "model", "index", "serve", "act"}) {
        EXPECT_EQ(sandbox.apply({step, Json::object()}).status, zetta::agent::ApplyStatus::Ok) << step;
    }
    // A tool nobody registered is still refused.
    EXPECT_EQ(sandbox.apply({"drop_everything", Json::object()}).status,
              zetta::agent::ApplyStatus::UnknownTool);

    // With no runner, a listed tool stays a marker and calls nothing.
    calls.clear();
    zetta::agent::Sandbox markers = zetta::agent::chat_sandbox(agent);
    EXPECT_EQ(markers.apply({"get_run_report", Json::object()}).status, zetta::agent::ApplyStatus::Ok);
    EXPECT_TRUE(calls.empty());
}

// ---- the store ---------------------------------------------------------------------------

TEST(RunProposalStore, RoundTripsAndRefusesMalformedRecords) {
    zetta::cluster::ReplicationManager replication{"store-node"};
    zetta::pipeline::RunProposalStore store(replication);
    zetta::pipeline::RunProposal proposal;
    proposal.id = "p-1";
    proposal.message = "predict churn";
    proposal.proposed_at = "2026-10-21T00:00:00.000Z";
    proposal.preview = {{"project", "churn · 1234"}, {"creates_project", true}};
    std::string error;
    ASSERT_TRUE(store.put(proposal, &error)) << error;
    const auto read = store.get("p-1");
    ASSERT_TRUE(read);
    EXPECT_EQ(read->status, zetta::pipeline::RunProposalStatus::Proposed);
    EXPECT_EQ(read->preview, proposal.preview);

    zetta::pipeline::RunProposal newer = proposal;
    newer.id = "p-2";
    newer.proposed_at = "2026-10-21T00:00:01.000Z";
    newer.status = zetta::pipeline::RunProposalStatus::Rejected;
    ASSERT_TRUE(store.put(newer, &error)) << error;
    const auto listed = store.list();
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0].id, "p-2") << "newest first";

    zetta::pipeline::RunProposal unnamed = proposal;
    unnamed.message.clear();
    EXPECT_FALSE(store.put(unnamed, &error));
    EXPECT_NE(error.find("/message"), std::string::npos) << error;
    EXPECT_FALSE(zetta::pipeline::run_proposal_from_json(
        Json{{"id", "x"}, {"message", "m"}, {"status", "maybe"}}, &error));
    EXPECT_NE(error.find("/status"), std::string::npos) << error;
}

}  // namespace
