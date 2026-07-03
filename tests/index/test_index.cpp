#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/index/in_memory_index_backend.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_backend.hpp"
#include "zetta/index/index_registry.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::index::Index;
using zetta::index::IndexBackend;
using zetta::index::IndexDocument;
using zetta::index::IndexKind;
using zetta::index::IndexPartition;
using zetta::index::IndexQuery;
using zetta::index::IndexRegistry;
using zetta::index::IndexSpec;
using zetta::index::IndexStatusCode;
using zetta::index::IndexValue;
using zetta::index::InMemoryIndexBackend;
using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A fully-populated index value (everything but `created`, which the node fills).
IndexValue sample_value() {
    IndexValue value;
    value.table = "accounts";
    value.kind = IndexKind::Search;
    value.backend = std::string(InMemoryIndexBackend::kName);
    value.fields = {"plan", "region"};
    return value;
}

// Two partitions of account rows, the unit a backend builds over.
std::vector<IndexPartition> sample_partitions() {
    IndexPartition chunk0;
    chunk0.id = "000000";
    chunk0.documents = {
        IndexDocument{"row1", {{"plan", "pro"}, {"region", "us"}}},
        IndexDocument{"row2", {{"plan", "free"}, {"region", "eu"}}},
        IndexDocument{"row3", {{"plan", "pro"}, {"region", "eu"}}},
    };
    IndexPartition chunk1;
    chunk1.id = "000001";
    chunk1.documents = {
        IndexDocument{"row4", {{"plan", "pro"}, {"region", "us"}}},
        // A row missing an indexed field is tolerated (skipped for that field).
        IndexDocument{"row5", {{"region", "us"}}},
    };
    return {chunk0, chunk1};
}

// An IR `index` step carrying the index definition in its attributes.
zetta::ir::Node index_step(nlohmann::json attributes) {
    return zetta::ir::Node{"step-index", zetta::ir::StepKind::Index,
                           "build index", std::move(attributes)};
}

}  // namespace

// ----- IndexKind + IndexSpec -------------------------------------------------

TEST(IndexKind, NamesRoundTrip) {
    EXPECT_EQ(zetta::index::to_string(IndexKind::Search), "search");
    EXPECT_EQ(zetta::index::to_string(IndexKind::Vector), "vector");
    EXPECT_EQ(zetta::index::to_string(IndexKind::Feature), "feature");
    EXPECT_EQ(zetta::index::index_kind_from_name("vector"), IndexKind::Vector);
    EXPECT_FALSE(zetta::index::index_kind_from_name("nope").has_value());
}

TEST(IndexSpec, KeyIsBackendIndependentAndFieldOrdered) {
    IndexSpec a{"accounts", IndexKind::Search, "memory_search", {"plan", "region"}};
    IndexSpec b{"accounts", IndexKind::Search, "other_backend", {"plan", "region"}};
    // backend does NOT select the index — same table/kind/fields => same key.
    EXPECT_EQ(a.key(), b.key());
    // Field order is part of identity.
    IndexSpec c{"accounts", IndexKind::Search, "memory_search", {"region", "plan"}};
    EXPECT_NE(a.key(), c.key());
    // kind and table are part of identity.
    IndexSpec d{"accounts", IndexKind::Vector, "memory_search", {"plan", "region"}};
    EXPECT_NE(a.key(), d.key());
}

// ----- index_spec_from_node (the IR `index` step boundary) -------------------

TEST(IndexSpecFromNode, ExtractsAValidIndexStep) {
    const auto spec = zetta::index::index_spec_from_node(index_step(
        {{"table", "accounts"},
         {"kind", "search"},
         {"backend", "memory_search"},
         {"fields", {"plan", "region"}}}));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->table, "accounts");
    EXPECT_EQ(spec->kind, IndexKind::Search);
    EXPECT_EQ(spec->backend, "memory_search");
    ASSERT_EQ(spec->fields.size(), 2u);
    EXPECT_EQ(spec->fields[0], "plan");
}

TEST(IndexSpecFromNode, RejectsMalformedOrNonIndexNodes) {
    // Not an index step.
    EXPECT_FALSE(zetta::index::index_spec_from_node(zetta::ir::Node{
        "s", zetta::ir::StepKind::Model, "m",
        {{"table", "t"},
         {"kind", "search"},
         {"backend", "memory_search"},
         {"fields", {"f"}}}}));
    // Missing table.
    EXPECT_FALSE(zetta::index::index_spec_from_node(index_step(
        {{"kind", "search"}, {"backend", "memory_search"}, {"fields", {"f"}}})));
    // Empty table.
    EXPECT_FALSE(zetta::index::index_spec_from_node(index_step(
        {{"table", ""},
         {"kind", "search"},
         {"backend", "memory_search"},
         {"fields", {"f"}}})));
    // Unknown kind.
    EXPECT_FALSE(zetta::index::index_spec_from_node(index_step(
        {{"table", "t"},
         {"kind", "bogus"},
         {"backend", "memory_search"},
         {"fields", {"f"}}})));
    // Empty backend.
    EXPECT_FALSE(zetta::index::index_spec_from_node(index_step(
        {{"table", "t"}, {"kind", "search"}, {"backend", ""}, {"fields", {"f"}}})));
    // Empty fields array.
    EXPECT_FALSE(zetta::index::index_spec_from_node(index_step(
        {{"table", "t"},
         {"kind", "search"},
         {"backend", "memory_search"},
         {"fields", nlohmann::json::array()}})));
    // Duplicate fields.
    EXPECT_FALSE(zetta::index::index_spec_from_node(index_step(
        {{"table", "t"},
         {"kind", "search"},
         {"backend", "memory_search"},
         {"fields", {"f", "f"}}})));
    // Non-string field.
    EXPECT_FALSE(zetta::index::index_spec_from_node(index_step(
        {{"table", "t"},
         {"kind", "search"},
         {"backend", "memory_search"},
         {"fields", {1}}})));
}

// ----- IndexRegistry (the resolution seam) -----------------------------------

TEST(IndexRegistry, AddFindHasAndDeduplicatesNames) {
    IndexRegistry registry;
    EXPECT_TRUE(registry.empty());
    registry.add(std::make_shared<InMemoryIndexBackend>());
    EXPECT_EQ(registry.size(), 1u);
    EXPECT_TRUE(registry.has("memory_search"));
    EXPECT_FALSE(registry.has("nope"));
    ASSERT_TRUE(registry.find("memory_search"));
    EXPECT_EQ(registry.find("memory_search")->kind(), IndexKind::Search);

    // A null backend and a duplicate name are both ignored.
    registry.add(nullptr);
    registry.add(std::make_shared<InMemoryIndexBackend>());
    EXPECT_EQ(registry.size(), 1u);
}

TEST(IndexRegistry, ResolvesBySpecAndByIrStep) {
    IndexRegistry registry;
    registry.add(std::make_shared<InMemoryIndexBackend>());

    // Resolve by spec: name + kind both match.
    const IndexSpec spec = sample_value().spec();
    auto backend = registry.resolve(spec);
    ASSERT_TRUE(backend);
    EXPECT_EQ(backend->name(), "memory_search");

    // Resolve straight from the IR `index` step — the acceptance path.
    auto from_node = registry.resolve(index_step(
        {{"table", "accounts"},
         {"kind", "search"},
         {"backend", "memory_search"},
         {"fields", {"plan"}}}));
    ASSERT_TRUE(from_node);
    EXPECT_EQ(from_node->name(), "memory_search");
}

TEST(IndexRegistry, DoesNotResolveOnUnknownBackendOrKindMismatch) {
    IndexRegistry registry;
    registry.add(std::make_shared<InMemoryIndexBackend>());

    // Unknown backend name.
    IndexSpec unknown{"accounts", IndexKind::Search, "vector_hnsw", {"plan"}};
    EXPECT_FALSE(registry.resolve(unknown));

    // Name matches a registered backend, but it implements a different kind
    // (memory_search is Search, not Vector) — not a resolution.
    IndexSpec mismatch{"accounts", IndexKind::Vector, "memory_search", {"plan"}};
    EXPECT_FALSE(registry.resolve(mismatch));

    // A non-index / malformed IR node resolves to nothing.
    EXPECT_FALSE(registry.resolve(index_step(nlohmann::json::object())));
}

// ----- InMemoryIndexBackend (build over partitions / query / drop) -----------

TEST(InMemoryIndexBackend, BuildsQueriesAndDropsOverPartitions) {
    InMemoryIndexBackend backend;
    const IndexSpec spec = sample_value().spec();

    ASSERT_TRUE(backend.build(spec, sample_partitions()).ok());
    EXPECT_EQ(backend.built_count(), 1u);

    // plan == "pro" hits row1, row3, row4 (across both partitions), sorted.
    IndexQuery q;
    q.field = "plan";
    q.term = "pro";
    auto result = backend.query(spec, q);
    ASSERT_TRUE(result.status.ok()) << result.status.message;
    EXPECT_EQ(result.row_ids, (std::vector<std::string>{"row1", "row3", "row4"}));

    // plan == "free" hits only row2.
    q.term = "free";
    EXPECT_EQ(backend.query(spec, q).row_ids,
              (std::vector<std::string>{"row2"}));

    // A second indexed field works too (region == "eu" => row2, row3).
    IndexQuery region_q;
    region_q.field = "region";
    region_q.term = "eu";
    EXPECT_EQ(backend.query(spec, region_q).row_ids,
              (std::vector<std::string>{"row2", "row3"}));

    // top_k caps the result.
    q.term = "pro";
    q.top_k = 2;
    EXPECT_EQ(backend.query(spec, q).row_ids.size(), 2u);

    // A miss is an empty but successful result.
    q.term = "enterprise";
    q.top_k = 0;
    auto miss = backend.query(spec, q);
    EXPECT_TRUE(miss.status.ok());
    EXPECT_TRUE(miss.row_ids.empty());

    // Drop is idempotent.
    EXPECT_TRUE(backend.drop(spec).ok());
    EXPECT_EQ(backend.built_count(), 0u);
    EXPECT_TRUE(backend.drop(spec).ok());  // again, fine
}

TEST(InMemoryIndexBackend, RejectsBadSpecsAndQueries) {
    InMemoryIndexBackend backend;

    // A non-Search spec is unsupported by this backend.
    IndexSpec vector_spec{"accounts", IndexKind::Vector, "memory_search", {"v"}};
    EXPECT_EQ(backend.build(vector_spec, {}).code, IndexStatusCode::Unsupported);

    // No fields => invalid.
    IndexSpec no_fields{"accounts", IndexKind::Search, "memory_search", {}};
    EXPECT_EQ(backend.build(no_fields, {}).code,
              IndexStatusCode::InvalidArgument);

    // Query before build => not found.
    const IndexSpec spec = sample_value().spec();
    IndexQuery q;
    q.field = "plan";
    q.term = "pro";
    EXPECT_EQ(backend.query(spec, q).status.code, IndexStatusCode::NotFound);

    // Query a field the index does not cover => invalid argument.
    ASSERT_TRUE(backend.build(spec, sample_partitions()).ok());
    IndexQuery bad_field;
    bad_field.field = "not_indexed";
    bad_field.term = "x";
    EXPECT_EQ(backend.query(spec, bad_field).status.code,
              IndexStatusCode::InvalidArgument);
}

// End-to-end: resolve an IR index step to a backend, then build + query it.
TEST(IndexRegistry, ResolvedBackendBuildsAndAnswersAQuery) {
    IndexRegistry registry;
    registry.add(std::make_shared<InMemoryIndexBackend>());

    const zetta::ir::Node step = index_step(
        {{"table", "accounts"},
         {"kind", "search"},
         {"backend", "memory_search"},
         {"fields", {"plan", "region"}}});
    auto backend = registry.resolve(step);
    ASSERT_TRUE(backend);
    const IndexSpec spec = *zetta::index::index_spec_from_node(step);

    ASSERT_TRUE(backend->build(spec, sample_partitions()).ok());
    IndexQuery q;
    q.field = "plan";
    q.term = "pro";
    EXPECT_EQ(backend->query(spec, q).row_ids,
              (std::vector<std::string>{"row1", "row3", "row4"}));
}

// ----- The Index entity (value + schema boundary) ----------------------------

TEST(IndexValue, SchemaRejectsMalformedValuesAtTheBoundary) {
    using nlohmann::json;
    std::string error;

    EXPECT_TRUE(
        zetta::index::validate_index_value(zetta::index::to_json(sample_value()), &error))
        << error;

    // Missing required fields.
    EXPECT_FALSE(zetta::index::validate_index_value(
        json{{"kind", "search"}, {"backend", "b"}, {"fields", {"f"}}}, &error));
    // Empty table (minLength 1).
    EXPECT_FALSE(zetta::index::validate_index_value(
        json{{"table", ""},
             {"kind", "search"},
             {"backend", "b"},
             {"fields", {"f"}}},
        &error));
    // Unknown kind (enum).
    EXPECT_FALSE(zetta::index::validate_index_value(
        json{{"table", "t"},
             {"kind", "graph"},
             {"backend", "b"},
             {"fields", {"f"}}},
        &error));
    // Empty fields (minItems 1).
    EXPECT_FALSE(zetta::index::validate_index_value(
        json{{"table", "t"},
             {"kind", "search"},
             {"backend", "b"},
             {"fields", json::array()}},
        &error));
    // Duplicate fields (uniqueItems).
    EXPECT_FALSE(zetta::index::validate_index_value(
        json{{"table", "t"},
             {"kind", "search"},
             {"backend", "b"},
             {"fields", {"f", "f"}}},
        &error));
    // A stray field (additionalProperties: false).
    EXPECT_FALSE(zetta::index::validate_index_value(
        json{{"table", "t"},
             {"kind", "search"},
             {"backend", "b"},
             {"fields", {"f"}},
             {"surprise", 1}},
        &error));

    // parse_index_value rejects the same and returns nullopt with a reason.
    EXPECT_FALSE(zetta::index::parse_index_value(json{{"table", "t"}}, &error));
    EXPECT_FALSE(error.empty());
}

TEST(IndexValue, RoundTripsThroughJson) {
    const IndexValue original = sample_value();
    const auto parsed = zetta::index::parse_index_value(zetta::index::to_json(original));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->table, "accounts");
    EXPECT_EQ(parsed->kind, IndexKind::Search);
    EXPECT_EQ(parsed->backend, "memory_search");
    EXPECT_EQ(parsed->fields, (std::vector<std::string>{"plan", "region"}));
}

// ----- Node CRUD -------------------------------------------------------------

TEST(NodeIndex, CreateFindQueryDrop) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto index = node.create_index(sample_value());
    ASSERT_TRUE(index);
    EXPECT_EQ(index->table(), "accounts");
    EXPECT_EQ(index->kind(), IndexKind::Search);
    EXPECT_EQ(index->kind_name(), "search");
    EXPECT_EQ(index->backend(), "memory_search");
    EXPECT_EQ(index->fields(), (std::vector<std::string>{"plan", "region"}));
    EXPECT_FALSE(index->created().empty());

    // The entity hands the seam its spec.
    EXPECT_EQ(index->spec().key(), sample_value().spec().key());

    auto found = node.find_index(index->id());
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id(), index->id());
    EXPECT_EQ(node.index_count(), 1u);
    EXPECT_FALSE(node.find_index("nope"));

    EXPECT_TRUE(node.drop_index(index->id()));
    EXPECT_EQ(node.index_count(), 0u);
    EXPECT_FALSE(node.drop_index(index->id()));  // already gone
    node.stop();
}

TEST(NodeIndex, RejectsIncompleteSpecs) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    IndexValue no_table = sample_value();
    no_table.table.clear();
    EXPECT_FALSE(node.create_index(no_table));

    IndexValue no_backend = sample_value();
    no_backend.backend.clear();
    EXPECT_FALSE(node.create_index(no_backend));

    IndexValue no_fields = sample_value();
    no_fields.fields.clear();
    EXPECT_FALSE(node.create_index(no_fields));

    EXPECT_EQ(node.index_count(), 0u);
    node.stop();
}

TEST(NodeIndex, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto index = node.create_index(sample_value());
        ASSERT_TRUE(index);
        id = index->id();
        node.stop();
    }

    Node reloaded{config_for(tmp)};
    reloaded.start();
    EXPECT_EQ(reloaded.index_count(), 1u);
    auto index = reloaded.find_index(id);
    ASSERT_TRUE(index);
    EXPECT_EQ(index->table(), "accounts");
    EXPECT_EQ(index->backend(), "memory_search");
    EXPECT_EQ(index->fields(), (std::vector<std::string>{"plan", "region"}));
    reloaded.stop();
}

TEST(NodeIndex, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::util::new_uuid();
    std::string id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto index = node.create_index(sample_value());
        ASSERT_TRUE(index);
        id = index->id();
        node.tick();  // persist the CRDT document
        node.stop();
    }

    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int index_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("index:", 0) == 0) {
            ++index_keys;
            EXPECT_EQ(value.value("table", std::string{}), "accounts");
            EXPECT_EQ(value.value("kind", std::string{}), "search");
        }
    }
    EXPECT_EQ(index_keys, 1);

    auto index = node.find_index(id);
    ASSERT_TRUE(index);
    EXPECT_EQ(index->backend(), "memory_search");
    node.stop();
}

TEST(NodeIndex, InvalidIndexJsonOnDiskIsSkipped) {
    TempDir tmp;
    const std::filesystem::path bad = tmp.path() / zetta::util::new_uuid();
    std::filesystem::create_directories(bad);
    {
        std::ofstream out(bad / "index.json");
        out << R"({"id":"x","table":"accounts"})" << '\n';  // missing kind/backend/fields
    }

    Node node{config_for(tmp)};
    node.start();
    EXPECT_EQ(node.index_count(), 0u);  // skipped at the boundary, not loaded
    node.stop();
}

TEST(NodeIndex, RegistryVisibleViaSystemTable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto index = node.create_index(sample_value());
    ASSERT_TRUE(index);

    QueryService svc{node};
    auto sel = svc.run("SELECT * FROM system.zetta_indexes;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0]["index_id"].get<std::string>(), index->id());
    EXPECT_EQ(sel.rows[0]["table"].get<std::string>(), "accounts");
    EXPECT_EQ(sel.rows[0]["kind"].get<std::string>(), "search");
    EXPECT_EQ(sel.rows[0]["backend"].get<std::string>(), "memory_search");
    EXPECT_EQ(sel.rows[0]["fields"].get<std::vector<std::string>>(),
              (std::vector<std::string>{"plan", "region"}));
    EXPECT_EQ(sel.rows[0]["field_count"].get<int>(), 2);

    auto by_kind = svc.run(
        "SELECT index_id FROM system.zetta_indexes WHERE kind = 'search';", "");
    ASSERT_TRUE(by_kind.ok()) << by_kind.error;
    EXPECT_EQ(by_kind.rows.size(), 1u);
    node.stop();
}

TEST(NodeIndex, ReplicatesMetadataAcrossNodes) {
    TempDir dir_a, dir_b;
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
    cfg_a.listen_port = 43620;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43621;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43620"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    auto index = a.create_index(sample_value());
    ASSERT_TRUE(index);
    const std::string index_id = index->id();

    bool synced = false;
    for (int attempt = 0; attempt < 400; ++attempt) {
        a.tick();
        b.tick();
        if (b.find_index(index_id)) {
            synced = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(synced);

    auto b_index = b.find_index(index_id);
    ASSERT_TRUE(b_index);
    EXPECT_EQ(b_index->table(), "accounts");
    EXPECT_EQ(b_index->kind(), IndexKind::Search);
    EXPECT_EQ(b_index->backend(), "memory_search");
    EXPECT_EQ(b_index->fields(), (std::vector<std::string>{"plan", "region"}));

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
