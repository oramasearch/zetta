#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/collectives.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::CollectiveMessage;
using zetta::CollectiveOp;
using zetta::Collectives;
using zetta::decode_collective_message;
using zetta::encode_collective_message;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) return true;
        sleep_ms(25);
    }
    return false;
}

// One node of a loopback cluster: a Cluster plus the Collectives bound to it.
// Node ids are chosen so a test controls the deterministic root (lowest id).
struct Node {
    Node(std::string node_id, std::uint16_t port,
         const std::vector<std::string>& seeds)
        : cluster(std::move(node_id), make_config(port, seeds)),
          collectives(cluster) {}

    static ClusterConfig make_config(std::uint16_t port,
                                     const std::vector<std::string>& seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = seeds;
        return config;
    }

    void start() { cluster.start(); }
    void stop() { cluster.stop(); }

    Cluster cluster;
    Collectives collectives;
};

// Stand up a fully-connected loopback cluster on consecutive ports starting at
// `base_port`. zetta does no transitive peer discovery (a node connects only to
// who it dials + who dials it), so each node dials every earlier node — the
// symmetric handshake then makes the graph complete (every node ends connected
// to every other).
std::vector<std::unique_ptr<Node>> make_cluster(
    const std::vector<std::string>& ids, std::uint16_t base_port) {
    std::vector<std::unique_ptr<Node>> nodes;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
        }
        nodes.push_back(std::make_unique<Node>(
            ids[index], static_cast<std::uint16_t>(base_port + index), seeds));
    }
    for (auto& node : nodes) node->start();
    return nodes;
}

bool all_connected(const std::vector<std::unique_ptr<Node>>& nodes) {
    const std::size_t expected = nodes.size() - 1;
    return wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != expected) return false;
        }
        return true;
    });
}

void stop_all(std::vector<std::unique_ptr<Node>>& nodes) {
    for (auto& node : nodes) node->stop();
}

}  // namespace

// --- Message boundary: encode/decode + the schema ---------------------------

TEST(CollectiveMessage, RoundTrips) {
    CollectiveMessage message{
        .tag = "grad-epoch-7", .from = "node-a", .values = {1.5, -2.0, 3.25}};
    std::string error;
    const std::optional<CollectiveMessage> decoded =
        decode_collective_message(encode_collective_message(message), &error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->tag, "grad-epoch-7");
    EXPECT_EQ(decoded->from, "node-a");
    EXPECT_EQ(decoded->values, (std::vector<double>{1.5, -2.0, 3.25}));
}

TEST(CollectiveMessage, BarrierMessageHasNoValues) {
    CollectiveMessage message{.tag = "sync", .from = "node-a", .values = {}};
    const std::optional<CollectiveMessage> decoded =
        decode_collective_message(encode_collective_message(message));
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->values.empty());
}

TEST(CollectiveMessage, RejectsMalformedAtTheBoundary) {
    std::string error;
    EXPECT_FALSE(decode_collective_message("not json", &error));
    EXPECT_FALSE(error.empty());
    // Missing the required tag.
    EXPECT_FALSE(decode_collective_message(R"({"from":"a"})", &error));
    // Unknown field (additionalProperties: false).
    EXPECT_FALSE(decode_collective_message(R"({"tag":"t","extra":1})", &error));
    // Wrong type for values.
    EXPECT_FALSE(decode_collective_message(R"({"tag":"t","values":"x"})", &error));
    // A non-numeric value in the vector.
    EXPECT_FALSE(
        decode_collective_message(R"({"tag":"t","values":[1,"x"]})", &error));
}

TEST(CollectiveOpNames, RoundTrip) {
    for (const CollectiveOp op :
         {CollectiveOp::Sum, CollectiveOp::Product, CollectiveOp::Minimum,
          CollectiveOp::Maximum}) {
        EXPECT_EQ(zetta::collective_op_from_string(zetta::to_string(op)), op);
    }
    EXPECT_FALSE(zetta::collective_op_from_string("nonsense"));
}

// --- Single node: collectives degenerate cleanly ----------------------------

TEST(Collectives, SingletonClusterReducesLocally) {
    Cluster cluster{zetta::new_uuid(), ClusterConfig{}};
    Collectives collectives{cluster};
    cluster.start();

    const auto result =
        collectives.allreduce("solo", {2.0, 4.0, 8.0}, CollectiveOp::Sum,
                              std::chrono::seconds(1));
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, (std::vector<double>{2.0, 4.0, 8.0}));
    EXPECT_TRUE(collectives.barrier("solo-barrier", std::chrono::seconds(1)));

    cluster.stop();
}

// --- Multi-node loopback: the acceptance scenarios --------------------------

TEST(Collectives, AllreduceSumsPerNodeVectorsOnEveryNode) {
    // Three nodes with controlled ids so node "a" is the deterministic root.
    auto nodes = make_cluster({"node-a", "node-b", "node-c"}, 45200);
    ASSERT_TRUE(all_connected(nodes));

    // Each node contributes a different vector; every node must receive the sum.
    const std::vector<std::vector<double>> locals = {
        {1.0, 2.0, 3.0}, {10.0, 20.0, 30.0}, {100.0, 200.0, 300.0}};
    const std::vector<double> expected_sum = {111.0, 222.0, 333.0};

    std::vector<std::optional<std::vector<double>>> results(nodes.size());
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        threads.emplace_back([&, index] {
            results[index] = nodes[index]->collectives.allreduce(
                "epoch-1", locals[index], CollectiveOp::Sum,
                std::chrono::seconds(5));
        });
    }
    for (auto& thread : threads) thread.join();

    for (std::size_t index = 0; index < nodes.size(); ++index) {
        ASSERT_TRUE(results[index]) << "node " << index << " got no result";
        EXPECT_EQ(*results[index], expected_sum) << "node " << index;
    }

    stop_all(nodes);
}

TEST(Collectives, AllreduceMaxIsDeterministicAcrossNodes) {
    auto nodes = make_cluster({"node-a", "node-b", "node-c"}, 45210);
    ASSERT_TRUE(all_connected(nodes));

    const std::vector<std::vector<double>> locals = {
        {5.0, 1.0}, {2.0, 9.0}, {7.0, 4.0}};
    const std::vector<double> expected_max = {7.0, 9.0};

    std::vector<std::optional<std::vector<double>>> results(nodes.size());
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        threads.emplace_back([&, index] {
            results[index] = nodes[index]->collectives.allreduce(
                "max-round", locals[index], CollectiveOp::Maximum,
                std::chrono::seconds(5));
        });
    }
    for (auto& thread : threads) thread.join();

    for (const auto& result : results) {
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, expected_max);
    }

    stop_all(nodes);
}

TEST(Collectives, BroadcastDeliversTheRootVectorEverywhere) {
    auto nodes = make_cluster({"node-a", "node-b", "node-c"}, 45220);
    ASSERT_TRUE(all_connected(nodes));

    const std::vector<double> root_value = {42.0, 43.0};

    std::vector<std::optional<std::vector<double>>> results(nodes.size());
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        // Non-root callers pass a distinct vector that must be ignored.
        const std::vector<double> input =
            index == 0 ? root_value : std::vector<double>{double(index)};
        threads.emplace_back([&, index, input] {
            results[index] = nodes[index]->collectives.broadcast(
                "weights-v1", input, std::chrono::seconds(5));
        });
    }
    for (auto& thread : threads) thread.join();

    for (const auto& result : results) {
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, root_value);
    }

    stop_all(nodes);
}

TEST(Collectives, BarrierSynchronizesEveryNode) {
    auto nodes = make_cluster({"node-a", "node-b", "node-c"}, 45230);
    ASSERT_TRUE(all_connected(nodes));

    std::vector<char> released(nodes.size(), 0);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        threads.emplace_back([&, index] {
            if (nodes[index]->collectives.barrier("checkpoint",
                                                  std::chrono::seconds(5))) {
                released[index] = 1;
            }
        });
    }
    for (auto& thread : threads) thread.join();

    for (std::size_t index = 0; index < nodes.size(); ++index) {
        EXPECT_EQ(released[index], 1) << "node " << index << " not released";
    }

    stop_all(nodes);
}

TEST(Collectives, GatherCollectsEveryVectorAtTheRoot) {
    auto nodes = make_cluster({"node-a", "node-b", "node-c"}, 45240);
    ASSERT_TRUE(all_connected(nodes));

    const std::vector<std::vector<double>> locals = {
        {1.0}, {2.0}, {3.0}};

    std::vector<std::optional<std::vector<std::vector<double>>>> results(
        nodes.size());
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        threads.emplace_back([&, index] {
            results[index] = nodes[index]->collectives.gather(
                "collect", locals[index], std::chrono::seconds(5));
        });
    }
    for (auto& thread : threads) thread.join();

    // The root (node-a, lowest id) holds all three, ordered by node id.
    ASSERT_TRUE(results[0]);
    EXPECT_EQ(*results[0],
              (std::vector<std::vector<double>>{{1.0}, {2.0}, {3.0}}));
    // Non-root nodes contributed but hold nothing (classic MPI gather).
    ASSERT_TRUE(results[1]);
    EXPECT_TRUE(results[1]->empty());
    ASSERT_TRUE(results[2]);
    EXPECT_TRUE(results[2]->empty());

    stop_all(nodes);
}

TEST(Collectives, AllreduceFailsOnMismatchedVectorLengths) {
    auto nodes = make_cluster({"node-a", "node-b"}, 45250);
    ASSERT_TRUE(all_connected(nodes));

    std::optional<std::vector<double>> root_result;
    std::optional<std::vector<double>> peer_result;
    std::thread root([&] {
        root_result = nodes[0]->collectives.allreduce(
            "ragged", {1.0, 2.0}, CollectiveOp::Sum, std::chrono::seconds(3));
    });
    std::thread peer([&] {
        peer_result = nodes[1]->collectives.allreduce(
            "ragged", {1.0, 2.0, 3.0}, CollectiveOp::Sum,
            std::chrono::seconds(3));
    });
    root.join();
    peer.join();

    // The root detects the length mismatch and the round fails for it; the
    // contributing peer never receives a result and times out.
    EXPECT_FALSE(root_result);
    EXPECT_FALSE(peer_result);

    stop_all(nodes);
}
