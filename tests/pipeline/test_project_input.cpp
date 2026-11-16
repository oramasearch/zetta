#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_input.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/sha256.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::Node;
using zetta::NodeConfig;
using zetta::pipeline::ProjectInputKind;
using zetta::pipeline::ProjectInputSource;
using zetta::cluster::ReplicationManager;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

bool looks_like_iso8601(const std::string& stamp) {
    return !stamp.empty() && stamp.find('T') != std::string::npos &&
           stamp.back() == 'Z';
}

// Tick both nodes until `done` holds (or we give up). Mirrors the project +
// data replication tests: capture -> cluster gossip -> peer merge -> reconcile.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

constexpr const char* kIntent =
    "score every active account for churn risk and tell me why";

}  // namespace

TEST(ProjectInput, CreateFetchDropWithProvenance) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto project = node.create_project("churn");
    ASSERT_TRUE(project);
    EXPECT_EQ(project->input_count(), 0u);

    // An intent, pasted (no filename).
    auto intent = project->create_input(ProjectInputKind::Intent,
                                        ProjectInputSource::Pasted, kIntent,
                                        /*filename=*/{}, "user:steve");
    ASSERT_TRUE(intent);
    EXPECT_EQ(intent->kind(), ProjectInputKind::Intent);
    EXPECT_EQ(intent->source(), ProjectInputSource::Pasted);
    EXPECT_TRUE(intent->filename().empty());
    EXPECT_EQ(intent->created_by(), "user:steve");
    EXPECT_EQ(intent->project_id(), project->id());
    // content_hash is computed at the boundary, never trusted from the caller.
    EXPECT_EQ(intent->content_hash(), zetta::util::sha256_hex(kIntent));
    EXPECT_EQ(intent->size(), std::string(kIntent).size());
    EXPECT_TRUE(looks_like_iso8601(intent->created_at()));
    // The payload body is on disk and round-trips.
    EXPECT_TRUE(intent->has_body());
    ASSERT_TRUE(intent->read_payload().has_value());
    EXPECT_EQ(*intent->read_payload(), kIntent);

    // An artifact, uploaded (keeps its original filename).
    const std::string workflow = R"({"nodes":[],"connections":{}})";
    auto artifact = project->create_input(ProjectInputKind::Artifact,
                                          ProjectInputSource::Upload, workflow,
                                          "churn.json", "user:alice");
    ASSERT_TRUE(artifact);
    EXPECT_EQ(artifact->filename(), "churn.json");
    EXPECT_EQ(artifact->content_hash(), zetta::util::sha256_hex(workflow));

    EXPECT_EQ(project->input_count(), 2u);
    // Fetch by id.
    EXPECT_TRUE(project->find_input(intent->id()));
    EXPECT_TRUE(project->find_input(artifact->id()));
    EXPECT_FALSE(project->find_input("nope"));

    // Drop removes the record + its on-disk folder.
    EXPECT_TRUE(project->drop_input(intent->id()));
    EXPECT_EQ(project->input_count(), 1u);
    EXPECT_FALSE(project->find_input(intent->id()));
    EXPECT_FALSE(project->drop_input(intent->id()));  // already gone

    node.stop();
}

TEST(ProjectInput, CaptureIntentStoresWithProvenance) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto project = node.create_project("churn");
    ASSERT_TRUE(project);
    EXPECT_EQ(project->input_count(), 0u);

    // Capture a natural-language intent: kind=intent, source=pasted, no filename.
    auto intent = project->capture_intent(kIntent, "user:steve");
    ASSERT_TRUE(intent);
    EXPECT_EQ(intent->kind(), ProjectInputKind::Intent);
    EXPECT_EQ(intent->source(), ProjectInputSource::Pasted);
    EXPECT_TRUE(intent->filename().empty());
    EXPECT_EQ(intent->created_by(), "user:steve");
    EXPECT_EQ(intent->project_id(), project->id());
    // content_hash is computed at the boundary, never trusted from the caller.
    EXPECT_EQ(intent->content_hash(), zetta::util::sha256_hex(kIntent));
    EXPECT_EQ(intent->size(), std::string(kIntent).size());
    EXPECT_TRUE(looks_like_iso8601(intent->created_at()));

    // Retrievable with provenance, and the raw intent text round-trips off disk.
    EXPECT_EQ(project->input_count(), 1u);
    auto fetched = project->find_input(intent->id());
    ASSERT_TRUE(fetched);
    EXPECT_EQ(fetched->kind(), ProjectInputKind::Intent);
    ASSERT_TRUE(fetched->read_payload().has_value());
    EXPECT_EQ(*fetched->read_payload(), kIntent);

    node.stop();
}

TEST(ProjectInput, CaptureIntentRejectsEmpty) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto project = node.create_project("churn");
    ASSERT_TRUE(project);

    // Empty / whitespace-only intent carries no meaning: rejected, nothing stored.
    EXPECT_FALSE(project->capture_intent(""));
    EXPECT_FALSE(project->capture_intent("   \t\r\n  "));
    EXPECT_EQ(project->input_count(), 0u);

    // A real intent is still accepted afterward (rejection left no debris).
    auto intent = project->capture_intent(kIntent, "user:steve");
    ASSERT_TRUE(intent);
    EXPECT_EQ(project->input_count(), 1u);

    node.stop();
}

TEST(ProjectInput, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string project_id;
    std::string input_id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto project = node.create_project("churn");
        ASSERT_TRUE(project);
        project_id = project->id();
        auto input = project->create_input(ProjectInputKind::Artifact,
                                           ProjectInputSource::Upload, kIntent,
                                           "intent.txt", "user:steve");
        ASSERT_TRUE(input);
        input_id = input->id();
        node.stop();
    }

    // A fresh standalone node reads the input purely from disk (input.json +
    // payload), under its owning project.
    Node reloaded{config_for(tmp)};
    reloaded.start();
    auto project = reloaded.find_project("churn");
    ASSERT_TRUE(project);
    EXPECT_EQ(project->id(), project_id);
    ASSERT_EQ(project->input_count(), 1u);
    auto input = project->find_input(input_id);
    ASSERT_TRUE(input);
    EXPECT_EQ(input->kind(), ProjectInputKind::Artifact);
    EXPECT_EQ(input->source(), ProjectInputSource::Upload);
    EXPECT_EQ(input->filename(), "intent.txt");
    EXPECT_EQ(input->created_by(), "user:steve");
    EXPECT_EQ(input->content_hash(), zetta::util::sha256_hex(kIntent));
    ASSERT_TRUE(input->read_payload().has_value());
    EXPECT_EQ(*input->read_payload(), kIntent);
    reloaded.stop();
}

TEST(ProjectInput, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::util::new_uuid();
    std::string input_id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto project = node.create_project("churn");
        ASSERT_TRUE(project);
        auto input = project->create_input(ProjectInputKind::Intent,
                                           ProjectInputSource::Pasted, kIntent,
                                           {}, "user:steve");
        ASSERT_TRUE(input);
        input_id = input->id();
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document: the
    // input record is in the CRDT and the projection is rebuilt from it.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int input_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("input:", 0) == 0) {
            ++input_keys;
            EXPECT_EQ(value.value("kind", std::string{}), "intent");
            EXPECT_EQ(value.value("source", std::string{}), "pasted");
            EXPECT_EQ(value.value("content_hash", std::string{}),
                      zetta::util::sha256_hex(kIntent));
            EXPECT_EQ(value.value("created_by", std::string{}), "user:steve");
        }
    }
    EXPECT_EQ(input_keys, 1);

    auto project = node.find_project("churn");
    ASSERT_TRUE(project);
    auto input = project->find_input(input_id);
    ASSERT_TRUE(input);
    EXPECT_EQ(input->content_hash(), zetta::util::sha256_hex(kIntent));
    // Payload survives via disk on the same node.
    ASSERT_TRUE(input->read_payload().has_value());
    EXPECT_EQ(*input->read_payload(), kIntent);
    node.stop();
}

TEST(ProjectInput, ReplicatesToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43320;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43321;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43320"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Create a project + input on node A only.
    auto project_a = a.create_project("churn");
    ASSERT_TRUE(project_a);
    auto created = project_a->create_input(ProjectInputKind::Artifact,
                                           ProjectInputSource::Upload, kIntent,
                                           "churn.json", "user:alice");
    ASSERT_TRUE(created);

    // Node B converges to the same input record (metadata) under its project.
    const bool converged = converge(a, b, [&] {
        auto project_b = b.find_project("churn");
        return project_b && project_b->find_input(created->id()) &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    auto project_b = b.find_project("churn");
    ASSERT_TRUE(project_b);
    auto replicated = project_b->find_input(created->id());
    ASSERT_TRUE(replicated);
    EXPECT_EQ(replicated->kind(), ProjectInputKind::Artifact);
    EXPECT_EQ(replicated->source(), ProjectInputSource::Upload);
    EXPECT_EQ(replicated->filename(), "churn.json");
    EXPECT_EQ(replicated->created_by(), "user:alice");
    EXPECT_EQ(replicated->content_hash(), zetta::util::sha256_hex(kIntent));
    EXPECT_EQ(replicated->size(), std::string(kIntent).size());
    // The payload body does NOT transfer yet (a later slice, like LO bodies):
    // the record replicates, the bytes await body transfer.
    EXPECT_FALSE(replicated->has_body());
    EXPECT_FALSE(replicated->read_payload().has_value());

    // A drop on A also replicates to B.
    ASSERT_TRUE(project_a->drop_input(created->id()));
    const bool dropped = converge(a, b, [&] {
        auto project = b.find_project("churn");
        return project && !project->find_input(created->id());
    });
    EXPECT_TRUE(dropped);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}

TEST(ProjectInput, SkipsMalformedInputOnLoad) {
    TempDir tmp;
    std::string project_id;
    std::string good_id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto project = node.create_project("churn");
        ASSERT_TRUE(project);
        project_id = project->id();
        auto good = project->create_input(ProjectInputKind::Intent,
                                          ProjectInputSource::Pasted, kIntent);
        ASSERT_TRUE(good);
        good_id = good->id();

        // Hand-write a malformed input folder (unknown kind) next to the good
        // one: boundary validation should skip it on reload.
        const auto bad_dir = project->dir() / "inputs" / zetta::util::new_uuid();
        std::filesystem::create_directories(bad_dir);
        std::ofstream out(bad_dir / "input.json");
        out << R"({"kind":"nonsense","source":"pasted"})" << '\n';
        out.close();
        node.stop();
    }

    Node reloaded{config_for(tmp)};
    reloaded.start();
    auto project = reloaded.find_project("churn");
    ASSERT_TRUE(project);
    EXPECT_EQ(project->input_count(), 1u);  // only the valid input
    EXPECT_TRUE(project->find_input(good_id));
    reloaded.stop();
}
