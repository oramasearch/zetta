#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster.hpp"
#include "zetta/model.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/replicated_blob_store.hpp"
#include "zetta/replication.hpp"
#include "zetta/sha256.hpp"
#include "zetta/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::Model;
using zetta::ModelValue;
using zetta::Node;
using zetta::NodeConfig;
using zetta::ReplicatedBlobStore;
using zetta::ReplicationManager;
using zetta::sha256_hex;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A fully-populated model value (everything but `created` + `weights`, which the
// node / mutators fill), used to assert it round-trips through disk + the CRDT.
ModelValue sample_value() {
    ModelValue value;
    value.name = "churn-v1";
    value.kind = "classifier";
    value.project = "project-uuid";
    value.run = "run-uuid";
    value.feature_schema = {{"tenure_months", "number"},
                            {"plan", "string"},
                            {"monthly_charges", "number"}};
    value.metrics = {{"auc", 0.91}, {"f1", 0.82}};
    return value;
}

// Serialized weights with non-trivial bytes (exercises binary-safe transfer).
std::string sample_weights() {
    std::string data;
    data.reserve(50000);
    for (int i = 0; i < 50000; ++i) {
        data.push_back(static_cast<char>((i * 53 + 7) & 0xff));
    }
    return data;
}

}  // namespace

TEST(Model, CreateAttachWeightsQueryDrop) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // Create: requires a non-empty kind; stamps `created`; no weights yet.
    auto model = node.create_model(sample_value());
    ASSERT_TRUE(model);
    EXPECT_EQ(model->name(), "churn-v1");
    EXPECT_EQ(model->kind(), "classifier");
    EXPECT_EQ(model->project(), "project-uuid");
    EXPECT_EQ(model->run(), "run-uuid");
    EXPECT_FALSE(model->created().empty());
    EXPECT_FALSE(model->has_weights());
    EXPECT_FALSE(model->has_weights_body());
    EXPECT_DOUBLE_EQ(model->metrics().value("auc", 0.0), 0.91);

    // Attach weights: the body lands on disk + the fingerprint is recorded.
    const std::string weights = sample_weights();
    ASSERT_TRUE(model->set_weights(weights));
    ASSERT_TRUE(model->has_weights());
    EXPECT_EQ(model->weights()->size, weights.size());
    EXPECT_EQ(model->weights()->sha256, sha256_hex(weights));
    ASSERT_TRUE(model->has_weights_body());
    EXPECT_TRUE(std::filesystem::exists(model->weights_path()));
    ASSERT_TRUE(model->read_weights().has_value());
    EXPECT_EQ(*model->read_weights(), weights);

    // Query by id; models are keyed by id (no human name lookup).
    auto found = node.find_model(model->id());
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id(), model->id());
    EXPECT_EQ(node.model_count(), 1u);
    EXPECT_FALSE(node.find_model("nope"));

    // Drop removes it from the registry + disk.
    EXPECT_TRUE(node.drop_model(model->id()));
    EXPECT_EQ(node.model_count(), 0u);
    EXPECT_FALSE(node.drop_model(model->id()));  // already gone

    node.stop();
}

TEST(Model, RejectsEmptyKind) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    ModelValue value;  // kind left empty
    value.name = "no-kind";
    EXPECT_FALSE(node.create_model(value));
    EXPECT_EQ(node.model_count(), 0u);
    node.stop();
}

TEST(Model, PersistsWeightsAcrossRestartViaDisk) {
    TempDir tmp;
    const std::string weights = sample_weights();
    std::string id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto model = node.create_model(sample_value());
        ASSERT_TRUE(model);
        id = model->id();
        ASSERT_TRUE(model->set_weights(weights));
        node.stop();
    }

    // A fresh standalone node reads the model purely from disk (model.json +
    // weights), and the preflight integrity check confirms the body is sound.
    Node reloaded{config_for(tmp)};
    reloaded.start();
    EXPECT_EQ(reloaded.model_count(), 1u);
    auto model = reloaded.find_model(id);
    ASSERT_TRUE(model);
    EXPECT_EQ(model->kind(), "classifier");
    EXPECT_EQ(model->name(), "churn-v1");
    EXPECT_DOUBLE_EQ(model->metrics().value("f1", 0.0), 0.82);
    ASSERT_TRUE(model->has_weights());
    EXPECT_EQ(model->weights()->sha256, sha256_hex(weights));
    ASSERT_TRUE(model->has_weights_body());
    EXPECT_EQ(model->read_weights().value_or(""), weights);

    const auto& report = reloaded.integrity();
    EXPECT_EQ(report.models, 1u);
    EXPECT_EQ(report.model_weights_present, 1u);
    EXPECT_EQ(report.model_weights_missing, 0u);
    EXPECT_EQ(report.corrupt_model_weights, 0u);
    EXPECT_TRUE(report.ok());
    reloaded.stop();
}

TEST(Model, PersistsMetadataAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::new_uuid();
    std::string id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto model = node.create_model(sample_value());
        ASSERT_TRUE(model);
        id = model->id();
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document: the
    // model metadata is in the replicated document, projection rebuilt from it.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int model_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("model:", 0) == 0) {
            ++model_keys;
            EXPECT_EQ(value.value("kind", std::string{}), "classifier");
        }
    }
    EXPECT_EQ(model_keys, 1);

    auto model = node.find_model(id);
    ASSERT_TRUE(model);
    EXPECT_EQ(model->kind(), "classifier");
    EXPECT_EQ(model->name(), "churn-v1");
    node.stop();
}

TEST(Model, SchemaRejectsMalformedValuesAtTheBoundary) {
    using nlohmann::json;
    std::string error;

    // A well-formed value passes.
    EXPECT_TRUE(
        zetta::validate_model_value(zetta::to_json(sample_value()), &error))
        << error;

    // Missing the required `kind`.
    EXPECT_FALSE(zetta::validate_model_value(json{{"name", "x"}}, &error));
    // Empty `kind` (minLength 1).
    EXPECT_FALSE(zetta::validate_model_value(json{{"kind", ""}}, &error));
    // Non-numeric metric value.
    EXPECT_FALSE(zetta::validate_model_value(
        json{{"kind", "classifier"}, {"metrics", {{"auc", "high"}}}}, &error));
    // Weights missing the required sha256.
    EXPECT_FALSE(zetta::validate_model_value(
        json{{"kind", "classifier"}, {"weights", {{"size", 10}}}}, &error));
    // A stray top-level field (additionalProperties: false).
    EXPECT_FALSE(zetta::validate_model_value(
        json{{"kind", "classifier"}, {"surprise", 1}}, &error));

    // parse_model_value rejects the same and returns nullopt with a reason.
    EXPECT_FALSE(zetta::parse_model_value(json{{"name", "x"}}, &error));
    EXPECT_FALSE(error.empty());
}

TEST(Model, InvalidModelJsonOnDiskIsSkipped) {
    TempDir tmp;
    // Plant a folder with a malformed model.json (no kind) before start().
    const std::filesystem::path bad = tmp.path() / zetta::new_uuid();
    std::filesystem::create_directories(bad);
    {
        std::ofstream out(bad / "model.json");
        out << R"({"id":"x","name":"orphan"})" << '\n';
    }

    Node node{config_for(tmp)};
    node.start();
    EXPECT_EQ(node.model_count(), 0u);  // skipped at the boundary, not loaded
    node.stop();
}

TEST(Model, RegistryVisibleViaSystemTable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto model = node.create_model(sample_value());
    ASSERT_TRUE(model);
    ASSERT_TRUE(model->set_weights(sample_weights()));

    QueryService svc{node};
    auto sel = svc.run("SELECT * FROM system.zetta_models;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0]["model_id"].get<std::string>(), model->id());
    EXPECT_EQ(sel.rows[0]["kind"].get<std::string>(), "classifier");
    EXPECT_EQ(sel.rows[0]["name"].get<std::string>(), "churn-v1");
    EXPECT_TRUE(sel.rows[0]["has_weights"].get<bool>());
    EXPECT_TRUE(sel.rows[0]["weights_present"].get<bool>());
    EXPECT_EQ(sel.rows[0]["weights_size"].get<std::uintmax_t>(),
              sample_weights().size());

    // WHERE + projection over the registry.
    auto by_kind = svc.run(
        "SELECT model_id FROM system.zetta_models WHERE kind = 'classifier';",
        "");
    ASSERT_TRUE(by_kind.ok()) << by_kind.error;
    EXPECT_EQ(by_kind.rows.size(), 1u);

    node.stop();
}

TEST(Model, ReplicatesMetadataAndWeightsBodyAcrossNodes) {
    TempDir dir_a, dir_b;
    const std::string weights = sample_weights();

    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();
    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    ReplicatedBlobStore blobs_a{dir_a.path(), repl_a};
    ReplicatedBlobStore blobs_b{dir_b.path(), repl_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43600;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a, &blobs_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43601;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43600"};
    Cluster cluster_b{id_b, cfg_b, &repl_b, &blobs_b};
    cluster_b.start();

    // Create a model + attach weights on node A only.
    auto model = a.create_model(sample_value());
    ASSERT_TRUE(model);
    ASSERT_TRUE(model->set_weights(weights));
    const std::string model_id = model->id();

    // Node B should converge: the model metadata AND the weights body.
    const auto b_has_weights = [&] {
        auto m = b.find_model(model_id);
        if (!m || !m->has_weights_body()) return false;
        const auto bytes = m->read_weights();
        return bytes.has_value() && *bytes == weights;
    };
    bool synced = false;
    for (int attempt = 0; attempt < 600; ++attempt) {  // patient: body over TCP
        a.tick();
        b.tick();
        if (b_has_weights()) {
            synced = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(synced);

    // Metadata replicated, weights body verified + on B's disk.
    auto b_model = b.find_model(model_id);
    ASSERT_TRUE(b_model);
    EXPECT_EQ(b_model->kind(), "classifier");
    EXPECT_EQ(b_model->name(), "churn-v1");
    ASSERT_TRUE(b_model->has_weights());
    EXPECT_EQ(b_model->weights()->size, weights.size());
    EXPECT_EQ(b_model->weights()->sha256, sha256_hex(weights));
    EXPECT_EQ(sha256_hex(b_model->read_weights().value_or("")),
              sha256_hex(weights));

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
