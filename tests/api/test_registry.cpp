// The capability registry + main-thread bridge (docs/api/registry-design.md,
// #615). Also home of the first layer-1 MCP journeys
// (docs/testing/mcp-journeys.md): ordered capability calls with read-back
// assertions over a real Node.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/api/bridge.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/version.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Capability;
using zetta::api::CapabilityKind;
using zetta::api::Registry;

struct RegistryFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }

    // Spans land in the node's own ring (#640) — the same source the
    // system.traces / system.spans views and the GUI read (RULE-7).
    ApiResult call(const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    }
};

TEST_F(RegistryFixture, RegistersTheCoreCapabilities) {
    EXPECT_EQ(registry.size(), 6u);
    EXPECT_TRUE(registry.find("server_info"));
    EXPECT_TRUE(registry.find("sql"));
    EXPECT_FALSE(registry.find("no_such_tool"));
}

TEST_F(RegistryFixture, RejectsDuplicateAndHandlerlessEntries) {
    EXPECT_FALSE(registry.add(Capability{"server_info", "", "", {}, CapabilityKind::Read,
                                         {}, false, false,
                                         [](const nlohmann::json&, ApiContext&) {
                                             return ApiResult::success({});
                                         }}));
    EXPECT_FALSE(registry.add(Capability{"handlerless", "", "", {}, CapabilityKind::Read,
                                         {}, false, false, nullptr}));
}

TEST_F(RegistryFixture, UnknownCapabilityFailsClearly) {
    auto result = call("no_such_tool", nlohmann::json::object());
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "unknown_capability");
}

TEST_F(RegistryFixture, SchemaRejectsMalformedInputWithTheReason) {
    auto result = call("sql", nlohmann::json{{"wrong_field", 1}});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "invalid_input");
    EXPECT_FALSE(result.error.message.empty());
}

TEST_F(RegistryFixture, UnauthenticatedLocalCallsAreAllowedAndAudited) {
    const auto audit_before = node.audit_event_count();
    auto result = call("server_info", nlohmann::json::object());
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("version"), zetta::kVersion);
    EXPECT_GT(node.audit_event_count(), audit_before);
    const auto events = node.audit_events();
    EXPECT_EQ(events.back()->value().target, "api:server_info");
    EXPECT_EQ(events.back()->value().actor, "local");
}

TEST_F(RegistryFixture, KeyWithoutCapabilityIsDeniedAndAudited) {
    auto principal = node.create_principal(
        "api tester", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    auto key = principal->create_key(zetta::governance::unrestricted_scope());
    ASSERT_TRUE(key);
    ApiContext context{node, nullptr, key->id()};
    auto result = registry.call("server_info", nlohmann::json::object(), context);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "unauthorized");
    const auto events = node.audit_events();
    EXPECT_FALSE(events.back()->value().allowed);
}

TEST_F(RegistryFixture, SqlReadsTheSystemViews) {
    auto result =
        call("sql", nlohmann::json{{"statement", "SELECT * FROM system.datasets;"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_TRUE(result.value.contains("rows"));
}

// Journey: create_project -> read back through list_projects AND sql. Every
// mutating step gets a read-back (the journey discipline), and the mutation
// fires the activity hook exactly once.
TEST_F(RegistryFixture, JourneyCreateProjectThenReadBack) {
    std::atomic<int> activity_count{0};
    registry.set_activity_hook([&](const zetta::api::ActivityEvent& event) {
        EXPECT_EQ(event.capability, "create_project");
        EXPECT_EQ(event.actor, "local");
        ++activity_count;
    });

    auto created = call("create_project", nlohmann::json{{"name", "churn-q3"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(activity_count.load(), 1);

    auto listed = call("list_projects", nlohmann::json::object());
    ASSERT_TRUE(listed.ok);
    const auto projects = listed.value.at("projects");
    EXPECT_NE(std::find(projects.begin(), projects.end(), "churn-q3"),
              projects.end());

    // Negative branch of the journey: the duplicate is refused with a reason.
    auto duplicate = call("create_project", nlohmann::json{{"name", "churn-q3"}});
    EXPECT_FALSE(duplicate.ok);
    EXPECT_EQ(duplicate.error.code, "engine_error");
    EXPECT_EQ(activity_count.load(), 1);  // no hook on failure
}

// Every call is a span with intent + result shape, and the result carries the
// trace ids — the observability contract (docs/architecture/observability.md).
TEST_F(RegistryFixture, CallsAreSpansAndTracesReadBack) {
    auto created = call("create_project",
                        nlohmann::json{{"name", "traced"},
                                       {"intent", "verify observability"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.trace_id, trace.trace_id);
    EXPECT_EQ(created.span_id.size(), 16u);
    EXPECT_GE(created.duration_ms, 0.0);

    auto fetched = call("get_trace", nlohmann::json{{"trace_id", trace.trace_id}});
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    const auto& spans = fetched.value.at("spans");
    ASSERT_GE(spans.size(), 1u);
    bool found_intent = false;
    for (const auto& span : spans) {
        if (span.at("name") == "api:create_project") {
            EXPECT_EQ(span.at("attributes").at("intent"), "verify observability");
            EXPECT_TRUE(span.at("ok").get<bool>());
            found_intent = true;
        }
    }
    EXPECT_TRUE(found_intent);
}

// A failed call closes its span with the error, and the reserved intent field
// never reaches schema validation.
TEST_F(RegistryFixture, FailedCallsRecordErrorSpans) {
    auto denied = call("sql", nlohmann::json{{"intent", "probe"},
                                             {"statement", 42}});
    EXPECT_FALSE(denied.ok);
    auto fetched = call("get_trace", nlohmann::json{{"trace_id", trace.trace_id}});
    ASSERT_TRUE(fetched.ok);
    bool found_error_span = false;
    for (const auto& span : fetched.value.at("spans")) {
        if (span.at("name") == "api:sql" && !span.at("ok").get<bool>()) {
            EXPECT_NE(span.at("error").get<std::string>().find("invalid_input"),
                      std::string::npos);
            found_error_span = true;
        }
    }
    EXPECT_TRUE(found_error_span);
}

// Coroutine-first bridge: a runtime coroutine awaits run_on_owner and resumes
// after the owning loop drains — no caller-facing blocking primitives.
// (A free coroutine, not a lambda coroutine: parameters are copied into the
// frame, while a temporary lambda's captures dangle — the runtime.hpp caveat.)
namespace {
zetta::runtime::task<> await_bridge_work(zetta::api::MainThreadBridge& bridge,
                                         std::atomic<bool>& work_ran,
                                         std::atomic<bool>& coroutine_resumed) {
    co_await bridge.run_on_owner([&work_ran] { work_ran = true; });
    coroutine_resumed = true;
}
}  // namespace

TEST(MainThreadBridge, AwaitingCoroutineResumesAfterDrain) {
    zetta::api::MainThreadBridge bridge;
    std::atomic<bool> work_ran{false};
    std::atomic<bool> coroutine_resumed{false};

    zetta::runtime::spawn(await_bridge_work(bridge, work_ran, coroutine_resumed),
                          [] {});

    // The coroutine suspends on the event until the owning loop drains.
    for (int attempt = 0; attempt < 100 && bridge.pending() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(bridge.pending(), 1u);
    EXPECT_FALSE(work_ran.load());

    bridge.drain();
    for (int attempt = 0; attempt < 1000 && !coroutine_resumed; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(work_ran.load());
    EXPECT_TRUE(coroutine_resumed.load());
    EXPECT_EQ(bridge.pending(), 0u);
}

}  // namespace

// Journey (#640 acceptance): an agent runs a tool, then reads the trace back
// through BOTH surfaces — get_trace and the system.traces/system.spans SQL
// views — and sees intent, plan-level naming, duration, and result shape.
TEST_F(RegistryFixture, JourneyRunToolThenReadTraceContent) {
    auto created = call("create_project",
                        nlohmann::json{{"name", "trace-journey"},
                                       {"intent", "verify the trace loop"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    ASSERT_FALSE(created.trace_id.empty());

    // Read-back 1: the capability surface.
    auto fetched = call("get_trace", nlohmann::json{{"trace_id", created.trace_id}});
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    bool saw_call_span = false;
    for (const auto& span : fetched.value.at("spans")) {
        if (span.at("name") != "api:create_project") continue;
        saw_call_span = true;
        EXPECT_EQ(span.at("attributes").at("intent"), "verify the trace loop");
        EXPECT_GE(span.at("duration_ms").get<double>(), 0.0);
        EXPECT_TRUE(span.at("attributes").contains("result_shape"));
    }
    EXPECT_TRUE(saw_call_span);

    // Read-back 2: the SQL views over the SAME ring (RULE-7 — one source).
    auto traces = call(
        "sql", nlohmann::json{{"statement", "SELECT * FROM system.traces;"}});
    ASSERT_TRUE(traces.ok) << traces.error.message;
    bool saw_trace_row = false;
    for (const auto& row : traces.value.at("rows")) {
        if (row.at("trace_id") != created.trace_id) continue;
        saw_trace_row = true;
        EXPECT_EQ(row.at("intent"), "verify the trace loop");
        EXPECT_TRUE(row.at("ok").get<bool>());
        EXPECT_GE(row.at("span_count").get<int>(), 1);
    }
    EXPECT_TRUE(saw_trace_row);

    auto spans = call(
        "sql",
        nlohmann::json{{"statement", "SELECT name, intent, duration_ms FROM "
                                     "system.spans;"}});
    ASSERT_TRUE(spans.ok) << spans.error.message;
    EXPECT_FALSE(spans.value.at("rows").empty());
}
