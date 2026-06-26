#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "zetta/cluster.hpp"
#include "zetta/compute/distributed_scheduler.hpp"
#include "zetta/signal.hpp"
#include "zetta/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::Signal;
using zetta::SignalHandler;
using zetta::SignalKind;
using zetta::SignalResponse;
using zetta::compute::DistributedScheduler;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) return true;
        sleep_ms(25);
    }
    return false;
}

// The shared executor every node registers: square the integer in the payload.
// A stolen task must compute identically wherever it lands.
std::string square_executor(std::string_view /*method*/,
                            std::string_view payload) {
    const long value = std::stol(std::string(payload));
    return std::to_string(value * value);
}

}  // namespace

// --- Cross-node steal: an idle node does all the work of a source node --------

// Node A has **zero local workers** — a pure source that cannot compute
// anything itself. Node B is idle with a full pool. So if A's map() returns
// correct results at all, every task crossed the wire: B stole the pending tasks
// from A over the signal plane, ran them, and shipped the results back. That
// makes the cross-node steal deterministic to assert, not timing-dependent.
TEST(DistributedScheduler, IdleNodeStealsAllWorkFromASourceNode) {
    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 44200;
    config_a.port_range = 1;
    Cluster a{id_a, config_a};

    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 44201;
    config_b.port_range = 1;
    config_b.seeds = {"127.0.0.1:44200"};
    Cluster b{id_b, config_b};

    DistributedScheduler source{a, /*worker_count=*/0};  // can only hand out work
    DistributedScheduler helper{b, /*worker_count=*/4};  // idle, will steal it all
    source.set_executor(square_executor);
    helper.set_executor(square_executor);

    a.start();
    sleep_ms(100);
    b.start();
    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b.peer_count() == 1; }));

    source.start();  // register handlers + steal loops
    helper.start();

    std::vector<std::string> inputs;
    constexpr int kTasks = 20;
    for (int value = 0; value < kTasks; ++value) inputs.push_back(std::to_string(value));

    const std::vector<std::string> results = source.map("square", inputs);

    ASSERT_EQ(results.size(), inputs.size());
    for (int value = 0; value < kTasks; ++value) {
        EXPECT_EQ(results[value], std::to_string(value * value)) << value;
    }

    // The source ran nothing itself (no workers) and handed every task out;
    // the helper stole and executed all of them.
    EXPECT_EQ(source.stats().local_executed, 0u);
    EXPECT_EQ(source.stats().handed_out, static_cast<std::size_t>(kTasks));
    EXPECT_EQ(helper.stats().stolen_executed, static_cast<std::size_t>(kTasks));

    source.stop();
    helper.stop();
    b.stop();
    a.stop();
}

// Both nodes have workers: the work is shared, and the helper steals a real
// share of it. Correctness is deterministic; we only assert that *some* tasks
// crossed the wire (not an exact split, which depends on timing).
TEST(DistributedScheduler, WorkIsSharedAcrossNodes) {
    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 44210;
    config_a.port_range = 1;
    Cluster a{id_a, config_a};

    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 44211;
    config_b.port_range = 1;
    config_b.seeds = {"127.0.0.1:44210"};
    Cluster b{id_b, config_b};

    DistributedScheduler owner{a, /*worker_count=*/2};
    DistributedScheduler helper{b, /*worker_count=*/2};
    // A deliberately slow owner so the idle helper wins a share of the backlog.
    auto slow_square = [](std::string_view, std::string_view payload) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const long value = std::stol(std::string(payload));
        return std::to_string(value * value);
    };
    owner.set_executor(slow_square);
    helper.set_executor(slow_square);

    a.start();
    sleep_ms(100);
    b.start();
    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b.peer_count() == 1; }));
    owner.start();
    helper.start();

    std::vector<std::string> inputs;
    constexpr int kTasks = 200;
    for (int value = 0; value < kTasks; ++value) inputs.push_back(std::to_string(value));

    const std::vector<std::string> results = owner.map("square", inputs);

    ASSERT_EQ(results.size(), inputs.size());
    for (int value = 0; value < kTasks; ++value) {
        EXPECT_EQ(results[value], std::to_string(value * value)) << value;
    }
    // The helper stole and ran a non-trivial share; the owner ran the rest.
    EXPECT_GT(helper.stats().stolen_executed, 0u);
    EXPECT_GT(owner.stats().local_executed, 0u);
    EXPECT_EQ(owner.stats().handed_out, helper.stats().stolen_executed);

    owner.stop();
    helper.stop();
    b.stop();
    a.stop();
}

// --- Handler boundary (no networking needed) ---------------------------------

namespace {

// Records non-steal signals so the fallback wiring can be asserted.
class RecordingFallback : public SignalHandler {
public:
    SignalResponse on_request(const Signal& request) override {
        ++requests;
        return SignalResponse{.ok = true, .payload = "fallback:" + request.method};
    }
    int requests = 0;
};

Signal make_request(std::string method, std::string payload) {
    Signal signal;
    signal.kind = SignalKind::Request;
    signal.id = "corr";
    signal.method = std::move(method);
    signal.payload = std::move(payload);
    signal.from = "peer-node";
    return signal;
}

}  // namespace

TEST(DistributedScheduler, StealRequestPayloadIsValidatedAtTheBoundary) {
    Cluster cluster{zetta::new_uuid(), ClusterConfig{}};  // not started
    DistributedScheduler scheduler{cluster, /*worker_count=*/1};
    scheduler.set_executor(square_executor);

    // Malformed steal payloads are rejected with a reason, not acted on.
    EXPECT_FALSE(scheduler.on_request(make_request("steal", "not json")).ok);
    EXPECT_FALSE(scheduler.on_request(make_request("steal", R"({})")).ok)
        << "missing count";
    EXPECT_FALSE(scheduler.on_request(make_request("steal", R"({"count":0})")).ok)
        << "count must be >= 1";
    EXPECT_FALSE(
        scheduler.on_request(make_request("steal", R"({"count":1,"x":2})")).ok)
        << "no additional properties";

    // A well-formed steal against a node with no jobs returns an empty offer.
    const SignalResponse empty =
        scheduler.on_request(make_request("steal", R"({"count":4})"));
    EXPECT_TRUE(empty.ok) << empty.error;
    EXPECT_NE(empty.payload.find("\"tasks\":[]"), std::string::npos);

    // A steal_result for an unknown job is ignored (idempotent), not an error.
    const SignalResponse unknown = scheduler.on_request(
        make_request("steal_result", R"({"job":"nope","results":[]})"));
    EXPECT_TRUE(unknown.ok) << unknown.error;
}

TEST(DistributedScheduler, NonStealSignalsGoToTheFallbackHandler) {
    Cluster cluster{zetta::new_uuid(), ClusterConfig{}};
    DistributedScheduler scheduler{cluster, /*worker_count=*/1};
    RecordingFallback fallback;
    scheduler.set_fallback_handler(&fallback);

    const SignalResponse response =
        scheduler.on_request(make_request("other", "x"));
    EXPECT_TRUE(response.ok);
    EXPECT_EQ(response.payload, "fallback:other");
    EXPECT_EQ(fallback.requests, 1);

    // Without a fallback, a non-steal request gets the default "no handler".
    DistributedScheduler bare{cluster, /*worker_count=*/1};
    EXPECT_FALSE(bare.on_request(make_request("other", "x")).ok);
}
