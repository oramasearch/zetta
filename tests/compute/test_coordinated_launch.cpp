// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/signal.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/observability/trace.hpp"

// The coordinated cross-process SPMD launch seam (#601): the `run.launch`
// envelope boundary (schema-validated, non-throwing), the listener's
// validate-on-the-network-thread / execute-on-the-owner-thread split
// (on_task enqueues, drain runs), the runner registry, and the delegate
// chaining that lets it coexist with the telemetry exchange. The full
// cross-node co-run is proven end to end in
// tests/usecases/churn/test_churn_coordinated_launch.cpp.
namespace {

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::Signal;
using zetta::cluster::SignalHandler;
using zetta::cluster::SignalKind;
using zetta::cluster::SignalResponse;
using zetta::compute::CoordinatedLaunchListener;
using zetta::compute::CoordinatedLaunchRequest;
using Json = nlohmann::json;

// An unstarted local cluster: enough for the listener (it only needs the node
// id + send_task, and these tests never require delivery).
Cluster make_cluster(const std::string& name) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = 0;
    return Cluster(name, config);
}

CoordinatedLaunchRequest sample_request() {
    CoordinatedLaunchRequest request;
    request.id = "launch-1";
    request.kind = "churn_solution";
    request.initiator = "node-a";
    request.participants = {"node-a", "node-b", "node-c"};
    request.payload = Json{{"answer", 42}};
    return request;
}

Signal task_signal(std::string payload) {
    Signal task;
    task.kind = SignalKind::Task;
    task.method = std::string(CoordinatedLaunchListener::kMethod);
    task.payload = std::move(payload);
    task.from = "node-a";
    return task;
}

// Records every signal it receives — the stand-in for the telemetry exchange
// at the end of the handler chain.
struct RecordingHandler : SignalHandler {
    SignalResponse on_request(const Signal& request) override {
        (void)request;
        ++requests;
        return SignalResponse{.ok = true, .payload = "delegated", .error = {}};
    }
    void on_event(const Signal& event) override { (void)event; ++events; }
    void on_task(const Signal& task) override { (void)task; ++tasks; }
    int requests = 0;
    int events = 0;
    int tasks = 0;
};

// ---- The envelope boundary ---------------------------------------------------

TEST(CoordinatedLaunch, EnvelopeRoundTrips) {
    const CoordinatedLaunchRequest request = sample_request();
    const std::string wire = zetta::compute::encode_coordinated_launch(request);

    std::string error;
    const auto decoded = zetta::compute::decode_coordinated_launch(wire, &error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->id, request.id);
    EXPECT_EQ(decoded->kind, request.kind);
    EXPECT_EQ(decoded->initiator, request.initiator);
    EXPECT_EQ(decoded->participants, request.participants);
    EXPECT_EQ(decoded->payload, request.payload);
}

TEST(CoordinatedLaunch, DecodeRejectsMalformedEnvelopes) {
    const auto rejects = [](const std::string& wire) {
        std::string error;
        const auto decoded =
            zetta::compute::decode_coordinated_launch(wire, &error);
        EXPECT_FALSE(decoded) << "accepted: " << wire;
        EXPECT_FALSE(error.empty());
    };
    rejects("not json at all");
    rejects("[]");  // not an object
    rejects(Json{{"kind", "churn_solution"},
                 {"initiator", "node-a"},
                 {"participants", Json::array({"node-a"})},
                 {"payload", Json::object()}}
                .dump());  // missing id
    rejects(Json{{"id", "launch-1"},
                 {"kind", ""},
                 {"initiator", "node-a"},
                 {"participants", Json::array({"node-a"})},
                 {"payload", Json::object()}}
                .dump());  // empty kind
    rejects(Json{{"id", "launch-1"},
                 {"kind", "churn_solution"},
                 {"initiator", "node-a"},
                 {"participants", Json::array()},
                 {"payload", Json::object()}}
                .dump());  // empty roster
    rejects(Json{{"id", "launch-1"},
                 {"kind", "churn_solution"},
                 {"initiator", "node-a"},
                 {"participants", Json::array({"node-a"})},
                 {"payload", "not-an-object"}}
                .dump());  // payload must be an object
    rejects(Json{{"id", "launch-1"},
                 {"kind", "churn_solution"},
                 {"initiator", "node-a"},
                 {"participants", Json::array({"node-a"})},
                 {"payload", Json::object()},
                 {"extra", true}}
                .dump());  // unknown field
}

TEST(CoordinatedLaunch, ParticipantRankFindsThisNode) {
    const CoordinatedLaunchRequest request = sample_request();
    EXPECT_EQ(zetta::compute::participant_rank(request, "node-a"), 0u);
    EXPECT_EQ(zetta::compute::participant_rank(request, "node-c"), 2u);
    EXPECT_FALSE(zetta::compute::participant_rank(request, "node-x"));
}

// ---- The listener: queue on the signal thread, execute on drain ---------------

TEST(CoordinatedLaunch, QueuesValidTasksAndDrainsThroughTheKindRunner) {
    Cluster cluster = make_cluster("launch-listener");
    CoordinatedLaunchListener listener(cluster);

    std::vector<CoordinatedLaunchRequest> ran;
    listener.set_runner("churn_solution",
                        [&ran](const CoordinatedLaunchRequest& request) {
                            ran.push_back(request);
                            return CoordinatedLaunchListener::LaunchOutcome{
                                .ok = true, .detail = "ran"};
                        });

    const CoordinatedLaunchRequest request = sample_request();
    listener.on_task(
        task_signal(zetta::compute::encode_coordinated_launch(request)));
    EXPECT_EQ(listener.pending(), 1u);
    EXPECT_TRUE(ran.empty()) << "on_task must only enqueue, never execute";

    EXPECT_EQ(listener.drain(), 1u);
    EXPECT_EQ(listener.pending(), 0u);
    ASSERT_EQ(ran.size(), 1u);
    EXPECT_EQ(ran[0].id, request.id);
    EXPECT_EQ(ran[0].participants, request.participants);
}

TEST(CoordinatedLaunch, RejectsMalformedTaskAtTheBoundary) {
    Cluster cluster = make_cluster("launch-reject");
    CoordinatedLaunchListener listener(cluster);

    listener.on_task(task_signal("{\"not\": \"a launch\"}"));
    EXPECT_EQ(listener.pending(), 0u);
    EXPECT_EQ(listener.rejected(), 1u);
    EXPECT_EQ(listener.drain(), 0u);
}

TEST(CoordinatedLaunch, DropsLaunchWithNoRegisteredRunnerObservably) {
    Cluster cluster = make_cluster("launch-unknown-kind");
    CoordinatedLaunchListener listener(cluster);
    zetta::observability::SpanRecorder recorder;
    listener.set_span_recorder(&recorder);

    CoordinatedLaunchRequest request = sample_request();
    request.kind = "unregistered_kind";
    listener.on_task(
        task_signal(zetta::compute::encode_coordinated_launch(request)));
    EXPECT_EQ(listener.drain(), 0u);
    EXPECT_EQ(listener.pending(), 0u);

    // Observable, never silent: the drop is a failed run.launch span.
    const auto spans = recorder.recent();
    ASSERT_FALSE(spans.empty());
    EXPECT_EQ(spans[0].name, "run.launch:unregistered_kind");
    EXPECT_FALSE(spans[0].ok);
}

TEST(CoordinatedLaunch, RunnerFailureLandsOnTheDrainSpan) {
    Cluster cluster = make_cluster("launch-runner-fail");
    CoordinatedLaunchListener listener(cluster);
    zetta::observability::SpanRecorder recorder;
    listener.set_span_recorder(&recorder);

    listener.set_runner("churn_solution",
                        [](const CoordinatedLaunchRequest&) {
                            return CoordinatedLaunchListener::LaunchOutcome{
                                .ok = false, .detail = "gate blocked the co-run"};
                        });
    listener.on_task(task_signal(
        zetta::compute::encode_coordinated_launch(sample_request())));
    EXPECT_EQ(listener.drain(), 1u);

    const auto spans = recorder.recent();
    ASSERT_FALSE(spans.empty());
    EXPECT_EQ(spans[0].name, "run.launch:churn_solution");
    EXPECT_FALSE(spans[0].ok);
    EXPECT_EQ(spans[0].error, "gate blocked the co-run");
}

TEST(CoordinatedLaunch, ForwardsEverythingElseToTheDelegate) {
    Cluster cluster = make_cluster("launch-delegate");
    RecordingHandler delegate;
    CoordinatedLaunchListener listener(cluster, &delegate);

    Signal event;
    event.kind = SignalKind::Event;
    event.method = "node.metrics";
    listener.on_event(event);
    EXPECT_EQ(delegate.events, 1);

    Signal other_task;
    other_task.kind = SignalKind::Task;
    other_task.method = "some.other.task";
    listener.on_task(other_task);
    EXPECT_EQ(delegate.tasks, 1);
    EXPECT_EQ(listener.pending(), 0u);

    Signal request;
    request.kind = SignalKind::Request;
    request.method = "sum";
    const SignalResponse response = listener.on_request(request);
    EXPECT_EQ(delegate.requests, 1);
    EXPECT_TRUE(response.ok);
    EXPECT_EQ(response.payload, "delegated");
}

TEST(CoordinatedLaunch, WithoutDelegateKeepsTheSafeHandlerDefaults) {
    Cluster cluster = make_cluster("launch-no-delegate");
    CoordinatedLaunchListener listener(cluster);

    Signal request;
    request.kind = SignalKind::Request;
    request.method = "sum";
    const SignalResponse response = listener.on_request(request);
    EXPECT_FALSE(response.ok);  // the "no handler" default, never a hang
}

}  // namespace
