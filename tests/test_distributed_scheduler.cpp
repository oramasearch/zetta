#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
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

// --- Elastic mid-job membership (#367) ---------------------------------------

// Killing a helper mid-job must not strand the tasks it holds. The owner detects
// the departed peer via cluster membership and re-queues its in-flight tasks, so
// a surviving worker re-executes them and map() still returns correct results. A
// deliberately long steal lease isolates the *membership* recovery path: the
// lease cannot fire within the test window, so the only thing that re-queues the
// killed helper's tasks is its disappearance from the cluster.
TEST(DistributedScheduler, KilledWorkerMidJobReexecutesAndJobCompletes) {
    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 44220;
    config_a.port_range = 1;
    // Drop a silent peer quickly: severing b below stops its io_context without a
    // clean socket close, so A learns b is gone via this idle timeout rather than
    // a prompt EOF (a real process crash closes its sockets and is detected even
    // faster). 1 s keeps the test brisk while still exercising the membership path.
    config_a.peer_idle_timeout_seconds = 1;
    Cluster a{id_a, config_a};

    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 44221;
    config_b.port_range = 1;
    config_b.seeds = {"127.0.0.1:44220"};
    Cluster b{id_b, config_b};

    // Owner A keeps real workers and a 60 s lease, so within the test only
    // membership detection (not the lease) can recover the killed helper's tasks.
    DistributedScheduler::Config owner_config;
    owner_config.steal_lease = std::chrono::seconds(60);
    DistributedScheduler owner{a, /*worker_count=*/2, owner_config};
    // A short steal RPC timeout so the helper's doomed result-shipping (to the
    // severed owner) gives up fast at teardown instead of waiting the default.
    DistributedScheduler::Config helper_config;
    helper_config.steal_timeout = std::chrono::milliseconds(250);
    DistributedScheduler helper{b, /*worker_count=*/4, helper_config};

    // The owner computes normally (it must finish after the helper dies). The
    // helper blocks inside every task it steals until released — so when we sever
    // it, it is provably holding in-flight tasks it can never return.
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto holding = std::make_shared<std::atomic<int>>(0);
    auto slow_square = [](std::string_view, std::string_view payload) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        const long value = std::stol(std::string(payload));
        return std::to_string(value * value);
    };
    auto blocking_square = [release, holding](std::string_view,
                                              std::string_view payload) {
        holding->fetch_add(1, std::memory_order_relaxed);
        while (!release->load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const long value = std::stol(std::string(payload));
        return std::to_string(value * value);
    };
    owner.set_executor(slow_square);
    helper.set_executor(blocking_square);

    a.start();
    sleep_ms(100);
    b.start();
    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b.peer_count() == 1; }));
    owner.start();
    helper.start();

    std::vector<std::string> inputs;
    constexpr int kTasks = 200;
    for (int value = 0; value < kTasks; ++value) inputs.push_back(std::to_string(value));

    // Run the job on a background thread so we can kill the helper mid-flight.
    std::vector<std::string> results;
    std::thread map_thread([&] { results = owner.map("square", inputs); });

    // Wait until the helper is provably holding stolen tasks (blocked inside them).
    ASSERT_TRUE(wait_for([&] { return holding->load() > 0; }));

    // Kill the helper: sever its network so it drops out of the cluster and can
    // never return the tasks it holds — exactly a node that failed mid-task. Its
    // scheduler is left running but isolated.
    b.stop();

    // The owner detects the departed peer, re-queues its tasks, and its own
    // workers finish them, so map() completes with correct results.
    map_thread.join();

    ASSERT_EQ(results.size(), inputs.size());
    for (int value = 0; value < kTasks; ++value) {
        EXPECT_EQ(results[value], std::to_string(value * value)) << value;
    }
    // Recovery came from membership detection, not the (60 s) lease backstop.
    EXPECT_GT(owner.stats().lost_node_requeued, 0u);

    *release = true;  // unblock the helper's stuck workers for a clean teardown
    helper.stop();
    owner.stop();
    a.stop();
}

// Adding a worker to a running job lets it help: a node that joins mid-job starts
// stealing and runs a real share, and the results stay correct. We assert the
// deterministic evidence of participation (the new node stole and ran tasks, and
// every handed-out task came back) rather than a flaky wall-clock speedup.
TEST(DistributedScheduler, WorkerJoiningMidJobParticipatesAndJobCompletes) {
    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 44230;
    config_a.port_range = 1;
    Cluster a{id_a, config_a};

    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 44231;
    config_b.port_range = 1;
    config_b.seeds = {"127.0.0.1:44230"};
    Cluster b{id_b, config_b};

    DistributedScheduler owner{a, /*worker_count=*/2};
    DistributedScheduler helper{b, /*worker_count=*/2};
    // A slow executor + a large job, so the owner is still grinding when the
    // helper joins and there is a backlog left to steal.
    auto slow_square = [](std::string_view, std::string_view payload) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        const long value = std::stol(std::string(payload));
        return std::to_string(value * value);
    };
    owner.set_executor(slow_square);
    helper.set_executor(slow_square);

    // The owner starts the job alone — no peers yet.
    a.start();
    owner.start();

    std::vector<std::string> inputs;
    constexpr int kTasks = 300;
    for (int value = 0; value < kTasks; ++value) inputs.push_back(std::to_string(value));

    std::vector<std::string> results;
    std::thread map_thread([&] { results = owner.map("square", inputs); });

    // Bring up the helper *after* the job is already running — the autoscaling
    // case: provision a node mid-flight and it joins the in-flight job.
    b.start();
    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b.peer_count() == 1; }));
    helper.start();

    map_thread.join();

    ASSERT_EQ(results.size(), inputs.size());
    for (int value = 0; value < kTasks; ++value) {
        EXPECT_EQ(results[value], std::to_string(value * value)) << value;
    }
    // The late-joining helper stole and ran a real share; the owner ran the rest;
    // every task it was handed came back (no re-queue for a healthy helper).
    EXPECT_GT(helper.stats().stolen_executed, 0u);
    EXPECT_GT(owner.stats().local_executed, 0u);
    EXPECT_EQ(owner.stats().handed_out, helper.stats().stolen_executed);

    helper.stop();
    owner.stop();
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
