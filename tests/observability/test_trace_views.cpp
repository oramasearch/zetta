// SPDX-License-Identifier: MIT

// The system.traces / system.spans read views over the node's span ring
// (docs/architecture/observability.md, #640): RULE-7 — the SQL surface, the
// list_traces/get_trace capabilities, and the GUI trace pane all read the ONE
// ring the Node owns.
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/service.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::test::TempDir;
using zetta::observability::ScopedSpan;
using zetta::observability::Span;
using zetta::observability::TraceContext;
using zetta::query::QueryService;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// One finished span straight into the node's ring (the API layer records the
// same way, via ScopedSpan against Node::span_recorder()).
void record_span(Node& node, const TraceContext& trace, const std::string& name,
                 nlohmann::json attributes = nlohmann::json::object(),
                 bool ok = true, const std::string& error = {}) {
    ScopedSpan span(&node.span_recorder(), trace, name);
    for (auto& [key, value] : attributes.items()) {
        span.set_attribute(key, value);
    }
    span.finish(ok, error);
}

}  // namespace

TEST(TraceViews, SpansViewExposesTheFourCaptures) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    QueryService svc{node};

    const auto trace = zetta::observability::make_root_trace();
    record_span(node, trace, "api:create_project",
                {{"intent", "spin up churn"},
                 {"plan", "create + capture"},
                 {"result_shape", {{"kind", "project"}}}});

    auto result = svc.run("SELECT * FROM system.spans;", "");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    const auto& row = result.rows[0];
    EXPECT_EQ(row.at("trace_id"), trace.trace_id);
    EXPECT_EQ(row.at("parent_span_id"), trace.span_id);
    EXPECT_EQ(row.at("name"), "api:create_project");
    EXPECT_EQ(row.at("intent"), "spin up churn");
    EXPECT_EQ(row.at("plan"), "create + capture");
    EXPECT_EQ(row.at("result_shape").at("kind"), "project");
    EXPECT_TRUE(row.at("ok").get<bool>());
    EXPECT_GE(row.at("duration_ms").get<double>(), 0.0);
    EXPECT_FALSE(row.at("started_at").get<std::string>().empty());

    node.stop();
}

TEST(TraceViews, TracesViewAggregatesOneRowPerTrace) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    QueryService svc{node};

    // Trace 1: a root span + a child, all green.
    const auto first = zetta::observability::make_root_trace();
    {
        ScopedSpan root(&node.span_recorder(), first, "api:sql");
        root.set_attribute("intent", "inspect datasets");
        const TraceContext child_parent{first.trace_id, root.span_id()};
        record_span(node, child_parent, "sql:select");
        root.finish(true);
    }
    // Trace 2: one failing span.
    const auto second = zetta::observability::make_root_trace();
    record_span(node, second, "api:create_project", {}, false,
                "engine_error: duplicate");

    auto result = svc.run("SELECT * FROM system.traces;", "");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 2u);

    const auto& ok_trace = result.rows[0];
    EXPECT_EQ(ok_trace.at("trace_id"), first.trace_id);
    EXPECT_EQ(ok_trace.at("name"), "api:sql");
    EXPECT_EQ(ok_trace.at("intent"), "inspect datasets");
    EXPECT_EQ(ok_trace.at("span_count").get<int>(), 2);
    EXPECT_TRUE(ok_trace.at("ok").get<bool>());
    EXPECT_EQ(ok_trace.at("error").get<std::string>(), "");

    const auto& failed_trace = result.rows[1];
    EXPECT_EQ(failed_trace.at("trace_id"), second.trace_id);
    EXPECT_EQ(failed_trace.at("span_count").get<int>(), 1);
    EXPECT_FALSE(failed_trace.at("ok").get<bool>());
    EXPECT_EQ(failed_trace.at("error").get<std::string>(),
              "engine_error: duplicate");

    // WHERE filters work over the computed rows (the shared select engine).
    auto filtered = svc.run("SELECT trace_id FROM system.spans WHERE name = "
                            "'sql:select';",
                            "");
    ASSERT_TRUE(filtered.ok()) << filtered.error;
    ASSERT_EQ(filtered.rows.size(), 1u);
    EXPECT_EQ(filtered.rows[0].at("trace_id"), first.trace_id);

    node.stop();
}

TEST(TraceViews, EmptyRingYieldsEmptyViews) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());
    QueryService svc{node};

    auto spans = svc.run("SELECT * FROM system.spans;", "");
    ASSERT_TRUE(spans.ok()) << spans.error;
    EXPECT_TRUE(spans.rows.empty());

    auto traces = svc.run("SELECT * FROM system.traces;", "");
    ASSERT_TRUE(traces.ok()) << traces.error;
    EXPECT_TRUE(traces.rows.empty());

    node.stop();
}

TEST(TraceViews, RecorderSnapshotIsOldestFirstAndBounded) {
    zetta::observability::SpanRecorder recorder(/*capacity=*/2);
    const auto trace = zetta::observability::make_root_trace();
    for (int index = 0; index < 3; ++index) {
        Span span;
        span.trace_id = trace.trace_id;
        span.span_id = zetta::observability::make_span_id();
        span.name = "span-" + std::to_string(index);
        recorder.record(span);
    }
    const auto snapshot = recorder.snapshot();
    ASSERT_EQ(snapshot.size(), 2u);  // bounded: oldest evicted
    EXPECT_EQ(snapshot[0].name, "span-1");
    EXPECT_EQ(snapshot[1].name, "span-2");
}
