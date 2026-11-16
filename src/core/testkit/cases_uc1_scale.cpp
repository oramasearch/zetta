// SPDX-License-Identifier: MIT

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../base64.hpp"  // zetta::detail::base64_decode_to_string
#include "cases.hpp"
#include "parquet_fixture_data.hpp"
#include "zetta/governance/access_key.hpp"
#include "zetta/cloud/add_nodes.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/cloud/burst_approval_gate.hpp"
#include "zetta/cloud/burst_identity.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/partition_rebalance.hpp"
#include "zetta/data/rendezvous_placement.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/input/cluster_bindings.hpp"
#include "zetta/input/datasets_bindings.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/cloud/local_provisioner.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ml/calibration.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/local_featurize_train_job.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/parquet/reader.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/data/table.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/pipeline/validation.hpp"

// The UC-1-at-scale end-to-end case (TC-12, #531/#502): the capstone scale test —
// the full cost-capped burst lifecycle (UC-4) around the distributed UC-1 pipeline.
// One canonical sim-Python program drives the whole flow over a real, REPLICATED
// loopback cluster (each member wired the way main() wires a live process:
// ReplicationManager + Node + ReplicatedBlobStore + Cluster):
//
//   load the saas-sales-conversations parquet as ONE PARTITIONED table placed
//   across the permanent members
//     -> burst: cluster.add_nodes(…, cost_cap=…) through the BurstApprovalGate
//        (an over-cap request is blocked deny-by-default and audited; the
//        within-cap burst provisions, joins, and partitions REBALANCE onto the
//        burst nodes — their bodies follow over the blob seam)
//       -> mint short-lived burst identities (key_issued audited)
//         -> featurize + train a calibrated GBDT on `outcome` compute-near-data
//            across the GROWN cluster (burst nodes train on their own shards)
//           -> explain (per-row TreeSHAP drivers) + rank (the at-risk list)
//             -> tear the burst down: drain partitions back to the permanent
//                members, revoke the burst identities (key_revoked audited) —
//                the model stays live for serving on every permanent node
//
// gated (RULE-1) and governed (key-scoped access + an audit trail). The same script
// is registered as a sim-Python example (examples.uc1_at_scale) so it doubles as the
// canonical usage example, and it is the program an operator runs for real in the
// headed app (start a node, add cloud nodes behind the consent gate, launch the case)
// — here the cloud provisioner is replaced by an injected LocalProvisioner over the
// standby loopback fleet so the case is hermetic and runs headless.
// See docs/use-cases/uc1-at-scale-e2e.md.
namespace zetta::testkit {
namespace {

using Json = nlohmann::json;
using compute::CpuDevice;
using compute::DataType;
using ddf::ColumnSchema;
using ddf::DataFrameSchema;
using ml::NearDataFeaturizeTrainResult;

// The canonical UC-1-at-scale program. One statement per line (the sim-Python
// frontend lowers each call to a typed step; refs become data-flow edges):
//   cluster.add_nodes -> act       load_dataset -> connect
//   train -> model                 explain -> serve            rank -> index
// The cost_cap= names the burst's spend ceiling in relative compute units
// (BurstCap::max_cost, #502) — the add runs through the BurstApprovalGate.
const std::string& uc1_at_scale_source() {
    static const std::string source =
        "nodes = cluster.add_nodes(10, provider='azure', cost_cap=12)\n"
        "conversations = load_dataset('saas-sales-conversations', columns=['messages', "
        "'avg_response_seconds', 'sentiment', 'questions_asked', 'outcome'])\n"
        "model = train(conversations, target='outcome')\n"
        "drivers = explain(model)\n"
        "at_risk = rank(model)\n";
    return source;
}

// The saas-sales-conversations feature columns are mostly integer; the two response
// signals are real. (The full dataset is ~3072-wide with embeddings — those are
// dropped at load; only the engineered columns + the binary `outcome` are kept.)
DataType dtype_for(const std::string& column) {
    if (column == "avg_response_seconds" || column == "sentiment") return DataType::Float64;
    return DataType::Int64;  // messages, questions_asked, outcome, conversation_id
}

// One synthetic saas-shaped conversation with a planted-but-imperfect signal mirroring
// the real fixture's structure (positive sentiment / more messages / faster responses /
// more questions => outcome 1), with ~9% of labels flipped — so a model learns a
// strong signal (AUC in (0.5, 1.0)) without being trivially separable. The real
// 12-row fixture seeds the dataset; this scales it to a manually-chosen size so the
// at-scale train + calibration are meaningful (the operator's real run loads the full
// ~100k dataset). Deterministic — no RNG (so single-node == multi-node holds).
Json synthetic_conversation(std::int64_t index) {
    const double sentiment =
        static_cast<double>((index * 37) % 201) / 100.0 - 1.0;  // [-1, 1]
    const int base = sentiment > 0.0 ? 1 : 0;
    const bool flip = (index % 11 == 0);
    const std::int64_t outcome = flip ? 1 - base : base;
    const std::int64_t messages =
        base ? 10 + (index * 13) % 25 : 1 + (index * 7) % 9;
    const double avg_response =
        base ? 5.0 + static_cast<double>((index * 9) % 60)
             : 200.0 + static_cast<double>((index * 11) % 520);
    const std::int64_t questions =
        base ? 2 + (index * 5) % 6 : ((index % 3 == 0) ? 1 : 0);
    return Json{{"messages", messages},
                {"avg_response_seconds", avg_response},
                {"sentiment", sentiment},
                {"questions_asked", questions},
                {"outcome", outcome}};
}

struct DatasetBuild {
    std::vector<Json> rows;     // one row per conversation, with a conversation_id key
    std::size_t real_rows = 0;  // how many came from the real parquet fixture
    bool embeddings_seen = false;  // the decoded parquet carried embedding_* leaves
    std::string error;
    bool ok() const { return error.empty(); }
};

// Load the embedded saas-sales-conversations parquet via the native reader, project to
// the script's columns (dropping the embeddings), stamp a conversation_id, then scale
// up to `target_rows` with synthetic saas-shaped rows. Proves the real parquet load
// path while keeping the at-scale train meaningful.
DatasetBuild build_saas_dataset(const std::vector<std::string>& columns,
                                std::size_t target_rows) {
    DatasetBuild out;
    const std::string bytes =
        zetta::detail::base64_decode_to_string(detail::kSaasSampleParquetBase64);
    if (bytes.empty()) {
        out.error = "embedded saas parquet fixture is empty";
        return out;
    }
    const parquet::ParquetRowsResult decoded = parquet::read_parquet_rows(bytes);
    if (!decoded.ok()) {
        out.error = "read_parquet_rows: " + decoded.error;
        return out;
    }
    std::int64_t id = 0;
    for (const Json& raw : decoded.rows) {
        if (raw.contains("embedding_0")) out.embeddings_seen = true;
        Json row;
        row["conversation_id"] = id;
        for (const std::string& column : columns) {
            if (raw.contains(column)) row[column] = raw.at(column);
        }
        out.rows.push_back(std::move(row));
        ++id;
    }
    out.real_rows = out.rows.size();
    while (out.rows.size() < target_rows) {
        const Json full = synthetic_conversation(id);
        Json row;
        row["conversation_id"] = id;
        for (const std::string& column : columns) {
            if (full.contains(column)) row[column] = full.at(column);
        }
        out.rows.push_back(std::move(row));
        ++id;
    }
    out.rows.resize(target_rows);
    return out;
}

// Modest GBDT knobs — enough trees to learn the signal, small enough to stay fast.
ml::GbdtParams gbdt_params() {
    ml::GbdtParams params;
    params.num_trees = 20;
    params.max_depth = 3;
    params.max_bins = 32;
    params.learning_rate = 0.2;
    return params;
}

ml::CalibrationConfig calibration_config(bool enabled) {
    ml::CalibrationConfig calibration;
    calibration.enabled = enabled;
    calibration.method = ml::CalibrationMethod::Platt;
    calibration.holdout_fraction = 0.2;
    return calibration;
}

std::string number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

std::size_t int_option(const TestOptions& options, const char* key, long fallback,
                       long floor, long ceiling) {
    return static_cast<std::size_t>(std::min<long>(
        ceiling, std::max<long>(floor, options.get_int(key, fallback))));
}

// The fixed partition count for the one partitioned conversations table. Chosen
// P >> the largest cluster this case stands up (7 nodes) so rendezvous placement
// gives every member a share and a rebalance moves ~P/N partitions, not all.
constexpr zetta::data::PartitionId kPartitionCount = 64;

// The case's transparent burst cost model: ONE relative compute unit per node, so
// a request's cost is simply its node count. Keeps the script's cost_cap=12
// against add_nodes(10) meaningfully inside the cap without a cloud price sheet
// (real pricing slots in as a different CostEstimator on the same gate).
double unit_burst_cost(const zetta::cloud::ProvisionRequest& request) {
    return static_cast<double>(request.count);
}

// Tick every node from the case thread — the owner loop for the setup /
// convergence phases — until `predicate()` holds. Patient (tests/CLAUDE.md):
// gossip, blob transfer, and reconcile need real time on a loopback cluster.
bool tick_until(LoopbackCluster& cluster, const std::function<bool()>& predicate,
                std::chrono::seconds patience = std::chrono::seconds(60)) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::size_t i = 0; i < cluster.size(); ++i) cluster.node(i).tick();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return predicate();
}

// The partitions of `table_id` this node's catalog currently shows, owned by
// `owner` (any owner when empty).
std::size_t partitions_visible(Node& node, const std::string& table_id,
                               const std::string& owner = {}) {
    std::size_t count = 0;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table_id) continue;
        if (!owner.empty()) {
            const std::vector<std::string>& owners = partition->owner_nodes();
            if (std::find(owners.begin(), owners.end(), owner) == owners.end()) continue;
        }
        ++count;
    }
    return count;
}

// Does `node` hold, on its own local disk, the body of every partition of
// `table_id` it owns — and own at least `minimum_owned` of them? The near-data
// load hard-fails on a missing owned body, so each phase gates on this before
// training (the bodies follow a placement change over the blob seam, #515).
bool owned_bodies_local(Node& node, const std::string& table_id,
                        std::size_t minimum_owned) {
    const std::shared_ptr<zetta::data::Database> database = node.find_database("data");
    if (!database) return false;
    std::size_t owned = 0;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table_id) continue;
        const std::vector<std::string>& owners = partition->owner_nodes();
        if (std::find(owners.begin(), owners.end(), node.node_id()) == owners.end()) {
            continue;
        }
        if (partition->blob_ref().empty()) continue;
        ++owned;
        if (!database->large_objects().read(partition->blob_ref())) return false;
    }
    return owned >= minimum_owned;
}

TestResult run_uc1_at_scale(const TestOptions& options, TestContext& context) {
    const std::size_t permanent = int_option(options, "nodes", 2, 1, 4);
    const std::size_t burst = int_option(options, "burst", 1, 1, 3);
    const std::size_t total = permanent + burst;
    const std::size_t rows = static_cast<std::size_t>(std::max<long>(
        static_cast<long>(std::max<std::size_t>(total, 12)),
        options.get_int("rows", 96)));
    const bool calibrate = options.get_bool("calibrate", true);

    // ---- 1) The script IS the spec: compile the canonical sim-Python program ----
    context.emit("UC-1-at-scale sim-Python program:\n" + uc1_at_scale_source());
    const input::FrontendResult compiled =
        input::PythonFrontend{}.compile(uc1_at_scale_source());
    if (!compiled.ok()) {
        return TestResult::fail("the UC-1-at-scale script did not compile: " +
                                compiled.error);
    }
    const ir::ExecutionGraph& graph = compiled.graph;
    context.emit("compiled to " + std::to_string(graph.node_count()) + " steps, " +
                 std::to_string(graph.edge_count()) + " edges");

    // ---- 2) Gate it (RULE-1 — no run without a passing correctness gate) --------
    const zetta::pipeline::ValidationReport gate = zetta::pipeline::default_gate().run(graph);
    if (!gate.passed() || gate.block_deploy) {
        std::string reason = "gate blocked deploy";
        for (const zetta::pipeline::CheckResult& check : gate.checks) {
            if (check.status == zetta::pipeline::CheckStatus::Fail) reason = check.name + ": " + check.reason;
        }
        return TestResult::fail("the correctness gate blocked the pipeline: " + reason);
    }
    context.emit("gate passed (" + std::to_string(gate.checks.size()) + " checks)");

    // ---- 3) Read the data shape, node count, and COST CAP from the script -------
    const std::shared_ptr<ir::Node> load_step = graph.find_node("load_dataset");
    const std::shared_ptr<ir::Node> add_step = graph.find_node("add_nodes");
    const std::shared_ptr<ir::Node> train_step = graph.find_node("train");
    if (!load_step || !add_step || !train_step) {
        return TestResult::fail("the lowered graph is missing an expected step");
    }
    const std::optional<input::DatasetLoadSpec> load_spec =
        input::dataset_load_spec(*load_step);
    if (!load_spec || load_spec->columns.empty()) {
        return TestResult::fail("the load_dataset step carries no column projection");
    }
    const std::size_t requested_nodes = input::add_nodes_count(*add_step).value_or(0);
    const std::optional<double> cost_cap = input::add_nodes_cost_cap(*add_step);
    if (!cost_cap) {
        return TestResult::fail("the add_nodes step names no cost_cap= — TC-12 is "
                                "the cost-capped burst");
    }
    const double burst_cost = static_cast<double>(requested_nodes);  // unit_burst_cost
    if (burst_cost > *cost_cap) {
        return TestResult::fail("the script's own burst (" + number(burst_cost) +
                                " units) exceeds its cost_cap (" + number(*cost_cap) +
                                ") — the canonical program must be within cap");
    }

    // The label is the train step's `target=` kwarg; the features are the rest.
    std::string label = "outcome";
    if (const Json bag = train_step->attributes.value("python", Json::object());
        bag.contains("kwargs") && bag["kwargs"].is_object() &&
        bag["kwargs"].contains("target") && bag["kwargs"]["target"].is_string()) {
        label = bag["kwargs"]["target"].get<std::string>();
    }
    std::vector<std::string> feature_columns;
    for (const std::string& column : load_spec->columns) {
        if (column != label) feature_columns.push_back(column);
    }
    if (feature_columns.empty()) {
        return TestResult::fail("no feature columns after removing the label '" + label + "'");
    }

    // ---- 4) Load the saas parquet (real reader) + scale to the chosen size ------
    const DatasetBuild dataset = build_saas_dataset(load_spec->columns, rows);
    if (!dataset.ok()) return TestResult::fail(dataset.error);
    if (!dataset.embeddings_seen) {
        return TestResult::fail("the saas parquet carried no embedding columns to drop");
    }
    context.emit("loaded the saas parquet (" + std::to_string(dataset.real_rows) +
                 " real rows, embeddings dropped) and scaled to " +
                 std::to_string(dataset.rows.size()) + " conversations");

    // The feature-table schema: the entity key + each projected column.
    DataFrameSchema schema;
    schema.columns.push_back(ColumnSchema{.name = "conversation_id", .dtype = DataType::Int64});
    for (const std::string& column : load_spec->columns) {
        schema.columns.push_back(ColumnSchema{.name = column, .dtype = dtype_for(column)});
    }

    // ---- 5) Stand up the REPLICATED cluster: permanent members + standby fleet --
    // Every node is wired the way main() wires a live process (ReplicationManager +
    // ReplicatedBlobStore behind the Node and the Cluster), so catalog changes
    // gossip and partition bodies follow ownership. Only the permanent members
    // announce themselves; the standby nodes are the hermetic stand-in for the
    // cloud capacity a real burst provisions.
    context.emit("bringing up " + std::to_string(permanent) + " permanent + " +
                 std::to_string(burst) + " standby loopback nodes (replicated)");
    LoopbackCluster cluster(total, LoopbackOptions{.replicated = true});
    if (!cluster.connected()) return TestResult::fail("cluster did not converge");
    for (std::size_t i = 0; i < permanent; ++i) cluster.announce_member(i);

    std::vector<std::string> permanent_ids;
    for (std::size_t i = 0; i < permanent; ++i) permanent_ids.push_back(cluster.node_id(i));

    // ---- 6) Load the parquet as ONE partitioned table across the permanent members
    Node& driver = cluster.node(0);
    std::shared_ptr<zetta::data::Database> database = driver.create_database("data");
    if (!database) return TestResult::fail("create_database failed on the driver");
    std::shared_ptr<zetta::data::Table> table = database->create_table("conversations");
    if (!table) return TestResult::fail("create_table failed on the driver");
    table->set_partitioner(std::make_shared<zetta::data::RendezvousPlacement>(
        kPartitionCount, /*replication_factor=*/1));
    for (const Json& row : dataset.rows) table->add_row(row);
    const zetta::data::PartitionWriteResult written =
        write_partitioned(driver, *database, *table, permanent_ids);
    if (!written.ok()) {
        return TestResult::fail("write_partitioned failed: " + written.error);
    }
    const std::string table_id = table->id();
    context.emit("wrote " + std::to_string(written.partitions.size()) +
                 " partitions across " + std::to_string(permanent) +
                 " permanent members");

    // Converge: every node sees the full partition catalog, and every permanent
    // member holds the bodies of the partitions it owns (pulled over the blob seam).
    const std::size_t partition_count = written.partitions.size();
    const bool loaded = tick_until(cluster, [&] {
        for (std::size_t i = 0; i < total; ++i) {
            if (partitions_visible(cluster.node(i), table_id) != partition_count) {
                return false;
            }
        }
        for (std::size_t i = 0; i < permanent; ++i) {
            if (!owned_bodies_local(cluster.node(i), table_id, /*minimum_owned=*/1)) {
                return false;
            }
        }
        return true;
    });
    if (!loaded) {
        return TestResult::fail("the partitioned load did not converge: catalog or "
                                "partition bodies missing on a permanent member");
    }
    context.emit("partitioned load converged on every node");

    // ---- 7) Governance: a scoped key authorizes the run; the trail is audited ---
    std::shared_ptr<zetta::governance::Principal> principal =
        driver.create_principal("uc1-pipeline", zetta::governance::PrincipalKind::Service);
    if (!principal) return TestResult::fail("create_principal failed");
    zetta::governance::Scope scope;
    scope.grant(zetta::governance::ResourceKind::Project, "uc1").grant(zetta::governance::ResourceKind::Database, "data");
    zetta::governance::CapabilitySet capabilities;
    capabilities.grant(zetta::governance::CapabilityDomain::Orchestration, "start_run")
        .grant(zetta::governance::CapabilityDomain::Data, "read");
    std::shared_ptr<zetta::governance::AccessKey> key = principal->create_key(scope, capabilities);
    if (!key) return TestResult::fail("create_key failed");
    // The key may start the run + read the project's data; it may not reach elsewhere.
    if (!driver.authorize(key->id(), zetta::governance::Operation{zetta::governance::CapabilityDomain::Orchestration, "start_run"})) {
        return TestResult::fail("the scoped key was denied orchestration:start_run");
    }
    if (!driver.authorize(key->id(), zetta::governance::Resource{zetta::governance::ResourceKind::Database, "data"})) {
        return TestResult::fail("the scoped key was denied access to its own database");
    }
    if (driver.authorize(key->id(), zetta::governance::Resource{zetta::governance::ResourceKind::Project, "unrelated"})) {
        return TestResult::fail("the scoped key reached a project outside its scope");
    }

    // ---- 8) The cost-capped burst — the lowered cluster.add_nodes(…, cost_cap=) -
    // The provider= is provenance only; the Provisioner is INJECTED (cloud stays
    // consent-gated). A LocalProvisioner over the standby fleet stands in for the
    // Azure burst; the BurstApprovalGate composition, the membership join, and the
    // rebalance onto the new members are the REAL engine paths either way.
    zetta::cloud::LocalProvisioner provisioner([&cluster, permanent]() {
        std::vector<zetta::cloud::ProvisionedNode> fleet;
        for (std::size_t i = permanent; i < cluster.size(); ++i) {
            fleet.push_back(zetta::cloud::ProvisionedNode{.node_id = cluster.node_id(i),
                                            .address = cluster.cluster(i).listen_address(),
                                            .capability = zetta::cloud::NodeCapability{}});
        }
        return fleet;
    });

    // First, the deny-by-default proof: a burst well past the script's own cap is
    // BLOCKED before the backend is touched, and the decision is audited.
    const std::string over_cap_source =
        "cluster.add_nodes(200, provider='azure', cost_cap=" + number(*cost_cap) + ")\n";
    const input::FrontendResult over_cap = input::PythonFrontend{}.compile(over_cap_source);
    if (!over_cap.ok()) return TestResult::fail("over-cap probe did not compile");
    const zetta::cloud::AddNodesResult blocked = input::run_add_nodes_step(
        *over_cap.graph.find_node("add_nodes"), driver, provisioner, {}, unit_burst_cost);
    if (blocked.satisfied || !blocked.members.empty()) {
        return TestResult::fail("an over-cap burst was not blocked by the approval gate");
    }
    if (driver.cluster_members().size() != permanent) {
        return TestResult::fail("a blocked burst changed the cluster membership");
    }
    context.emit("over-cap burst (200 nodes vs cap " + number(*cost_cap) +
                 ") blocked deny-by-default + audited");

    // The real burst: within the cap, through the same gate, Node-aware — the
    // provisioned members join the replicated membership AND the between-jobs
    // rebalance spreads partitions onto them.
    const zetta::cloud::AddNodesResult added = input::run_add_nodes_step(
        *add_step, driver, provisioner, {}, unit_burst_cost);
    if (added.members.empty()) {
        return TestResult::fail("cluster.add_nodes provisioned no burst nodes");
    }
    context.emit("cluster.add_nodes(" + std::to_string(requested_nodes) +
                 ", cost_cap=" + number(*cost_cap) + ") -> " +
                 std::to_string(added.members.size()) + " burst members (" +
                 number(burst_cost) + " units, best-effort on given hardware)");

    std::vector<std::string> grown_ids = permanent_ids;
    for (const zetta::cloud::ProvisionedNode& member : added.members) {
        grown_ids.push_back(member.node_id);
    }

    // The rebalance moved partitions onto the burst nodes; their bodies follow
    // over the blob seam. Converge before training: identical placement on every
    // node, every member (permanent AND burst) holding its owned bodies.
    const bool rebalanced = tick_until(cluster, [&] {
        std::size_t burst_owned = 0;
        for (const zetta::cloud::ProvisionedNode& member : added.members) {
            burst_owned += partitions_visible(driver, table_id, member.node_id);
        }
        if (burst_owned == 0) return false;
        for (std::size_t i = 0; i < total; ++i) {
            if (partitions_visible(cluster.node(i), table_id) != partition_count) {
                return false;
            }
            if (!owned_bodies_local(cluster.node(i), table_id, /*minimum_owned=*/1)) {
                return false;
            }
        }
        return true;
    });
    if (!rebalanced) {
        return TestResult::fail("partitions did not rebalance onto the burst nodes "
                                "(placement or bodies missing)");
    }
    std::size_t partitions_on_burst = 0;
    for (const zetta::cloud::ProvisionedNode& member : added.members) {
        partitions_on_burst += partitions_visible(driver, table_id, member.node_id);
    }
    context.emit(std::to_string(partitions_on_burst) + " of " +
                 std::to_string(partition_count) +
                 " partitions rebalanced onto the burst nodes, bodies present");

    // ---- 9) Mint short-lived burst identities (UC-4: temporary nodes) -----------
    zetta::cloud::BurstIdentityManager burst_identities(driver);
    const std::string burst_job = "uc1-at-scale-burst";
    const std::vector<zetta::cloud::BurstIdentity> minted =
        burst_identities.mint(burst_job, added.members, scope, capabilities);
    if (minted.size() != added.members.size()) {
        return TestResult::fail("burst identities were not minted for every burst node");
    }

    // ---- 10) SPMD near-data featurize -> train -> explain -> rank ---------------
    // The composed UC-1 pipeline (#530) across the GROWN cluster: every node —
    // permanent and burst — featurizes + trains over its OWN owned partitions.
    // The case thread has ticked the nodes (it is their owner loop), so while the
    // per-node job threads run it pumps the owner queues: the run's node writes
    // land through the owner gate exactly as under a live tick loop (#624).
    context.emit("featurizing + training on '" + label +
                 "' compute-near-data across " + std::to_string(total) + " nodes");
    ddf::FeaturizeSpec spec;
    spec.entity_column = "conversation_id";
    spec.account_features = feature_columns;
    spec.label_column = label;
    for (std::size_t i = 0; i < total; ++i) {
        cluster.node(i).attach_cluster(cluster.cluster(i), /*worker_count=*/2);
    }
    std::vector<std::unique_ptr<CpuDevice>> devices(total);
    std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs(total);
    std::vector<NearDataFeaturizeTrainResult> results(total);
    for (std::size_t i = 0; i < total; ++i) {
        devices[i] = std::make_unique<CpuDevice>();
        jobs[i] = cluster.node(i).create_job(zetta::compute::JobKind::Train);
    }
    {
        std::atomic<std::size_t> completed{0};
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < total; ++i) {
            threads.emplace_back([&, i] {
                results[i] = ml::run_near_data_featurize_train_job(
                    *cluster.node(i).job_coordinator(), cluster.node(i), *jobs[i],
                    table_id, schema, spec, gbdt_params(), *devices[i],
                    calibration_config(calibrate), "uc1-outcome-gbdt",
                    /*top_k=*/0, /*round_tag=*/"uc1.at_scale", std::chrono::seconds(30));
                completed.fetch_add(1);
            });
        }
        while (completed.load() < total) {
            for (std::size_t i = 0; i < total; ++i) cluster.node(i).drain_owner_work();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (std::thread& thread : threads) thread.join();
    }

    // ---- 11) Assert the at-scale result -----------------------------------------
    std::uint64_t total_rows_read = 0;
    std::uint64_t total_feature_rows = 0;
    std::uint64_t total_local_bytes = 0;
    std::uint64_t total_wire_bytes = 0;
    std::size_t total_explanations = 0;
    for (std::size_t i = 0; i < total; ++i) {
        const NearDataFeaturizeTrainResult& result = results[i];
        if (result.status != zetta::compute::JobStatus::Succeeded) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " near-data featurize-train did not succeed: " +
                                    jobs[i]->detail());
        }
        // The GBDT trained on the broadcast FEATURE TABLE — one row per conversation,
        // the engineered columns as X, identical on every node.
        if (result.feature_table.row_count() != dataset.rows.size()) {
            return TestResult::fail("node " + std::to_string(i) + " featurized " +
                                    std::to_string(result.feature_table.row_count()) +
                                    " of " + std::to_string(dataset.rows.size()) +
                                    " conversations");
        }
        if (result.feature_table.feature_names != feature_columns) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " feature-table columns diverged from the script");
        }
        if (result.feature_table.entities != results[0].feature_table.entities ||
            result.feature_table.features != results[0].feature_table.features) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " feature table differs from the root");
        }
        if (result.featurize_job_id.empty() ||
            !cluster.node(i).find_job(result.featurize_job_id)) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " featurize job was not recorded");
        }
        // The pipeline produced a model, per-row drivers, and a ranked at-risk list.
        if (result.train.feature_names != feature_columns) {
            return TestResult::fail("node " + std::to_string(i) + " feature ordering diverged");
        }
        if (result.train.top_driver.empty()) {
            return TestResult::fail("node " + std::to_string(i) + " surfaced no churn driver");
        }
        if (result.train.ranked.size() != dataset.rows.size()) {
            return TestResult::fail("node " + std::to_string(i) + " ranked " +
                                    std::to_string(result.train.ranked.size()) + " of " +
                                    std::to_string(dataset.rows.size()) + " conversations");
        }
        for (std::size_t r = 1; r < result.train.ranked.size(); ++r) {
            if (result.train.ranked[r - 1].risk < result.train.ranked[r].risk) {
                return TestResult::fail("node " + std::to_string(i) +
                                        " at-risk list is not ranked by risk descending");
            }
        }
        for (const ml::EntityScore& score : result.train.ranked) {
            if (score.risk < 0.0 || score.risk > 1.0) {
                return TestResult::fail("node " + std::to_string(i) + " risk out of [0,1]");
            }
        }
        // Every node converges on the IDENTICAL model + metrics + ranking (SPMD broadcast).
        if (result.train.model.to_json().dump() != results[0].train.model.to_json().dump()) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " model differs from the root (single-node != multi-node)");
        }
        if (result.train.ranked.size() != results[0].train.ranked.size()) {
            return TestResult::fail("node " + std::to_string(i) + " ranking differs from the root");
        }
        // Near-data instrumentation: this node read its OWN partitions off local disk
        // (the raw data never crossed the wire at train time — only the training
        // partials do), and on a multi-node cluster it read only its shard. This
        // holds on the BURST nodes too — the burst genuinely trained near data.
        if (result.partitions_read == 0 || result.local_bytes_read == 0) {
            return TestResult::fail("node " + std::to_string(i) + " read no owned partitions");
        }
        if (result.wire_bytes_moved == 0) {
            return TestResult::fail("node " + std::to_string(i) + " exchanged no partials");
        }
        if (total > 1 && result.rows_read >= dataset.rows.size()) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " read the whole dataset instead of its shard");
        }
        total_rows_read += result.rows_read;
        total_feature_rows += result.feature_rows;
        total_local_bytes += result.local_bytes_read;
        total_wire_bytes += result.wire_bytes_moved;
        total_explanations += result.train.explanations.size();
    }
    // The model learned a real signal off the (mostly) separable saas data.
    if (!(results[0].train.metrics.auc > 0.6)) {
        return TestResult::fail("the trained model did not learn the signal (AUC " +
                                number(results[0].train.metrics.auc) + ")");
    }
    if (calibrate && !results[0].train.calibrated) {
        return TestResult::fail("calibration was requested but the model was not calibrated");
    }
    // Every conversation was read near data across the shards exactly once, landed in
    // the feature table exactly once, and was explained exactly once.
    if (total_rows_read != dataset.rows.size() ||
        total_feature_rows != dataset.rows.size() ||
        total_explanations != dataset.rows.size()) {
        return TestResult::fail("rows read near data (" + std::to_string(total_rows_read) +
                                ") / featurized (" + std::to_string(total_feature_rows) +
                                ") / explained (" + std::to_string(total_explanations) +
                                ") != conversations (" + std::to_string(dataset.rows.size()) + ")");
    }
    // The produced Model is registered on the deterministic root (system.models);
    // it replicates to peers. The root is the lowest node id — not necessarily node 0.
    std::size_t root_index = 0;
    bool found_root = false;
    for (std::size_t i = 0; i < total; ++i) {
        if (results[i].is_root) {
            root_index = i;
            found_root = true;
            break;
        }
    }
    if (!found_root) return TestResult::fail("no node reported being the collective root");
    const std::string model_id = results[root_index].train.model_id;
    if (model_id.empty() || !cluster.node(root_index).find_model(model_id)) {
        return TestResult::fail("the trained Model was not registered on the root node");
    }

    // ---- 12) Tear the burst down (UC-4: temporary nodes; the model stays live) --
    // Drain the partitions back onto the permanent members (their bodies follow),
    // revoke the burst identities, and release the burst nodes through the same
    // provisioner seam. The trained model must still serve from the permanent
    // cluster afterwards.
    const zetta::data::RebalanceResult drained =
        zetta::data::rebalance_partitions(driver, permanent_ids, grown_ids);
    if (drained.moved == 0) {
        return TestResult::fail("tearing the burst down moved no partitions back");
    }
    const bool drained_back = tick_until(cluster, [&] {
        for (const zetta::cloud::ProvisionedNode& member : added.members) {
            if (partitions_visible(driver, table_id, member.node_id) != 0) return false;
        }
        for (std::size_t i = 0; i < permanent; ++i) {
            if (!owned_bodies_local(cluster.node(i), table_id, /*minimum_owned=*/1)) {
                return false;
            }
        }
        return true;
    });
    if (!drained_back) {
        return TestResult::fail("partitions did not drain back onto the permanent members");
    }
    const std::size_t revoked = burst_identities.revoke(burst_job);
    if (revoked != minted.size()) {
        return TestResult::fail("revoked " + std::to_string(revoked) + " of " +
                                std::to_string(minted.size()) + " burst identities");
    }
    for (const zetta::cloud::ProvisionedNode& member : added.members) {
        provisioner.teardown(member.node_id);  // given hardware: reported, not released
    }
    // The model stays live for serving: it resolves on every PERMANENT member
    // after the burst is gone (replicated state, not a copy on a burst node).
    const bool serving = tick_until(cluster, [&] {
        for (std::size_t i = 0; i < permanent; ++i) {
            if (!cluster.node(i).find_model(model_id)) return false;
        }
        return true;
    });
    if (!serving) {
        return TestResult::fail("the model does not resolve on every permanent member "
                                "after the burst teardown");
    }
    context.emit("burst torn down: partitions drained back, identities revoked, "
                 "model still serving on the permanent members");

    // ---- 13) Governance: the whole lifecycle was audited -------------------------
    int data_access = 0, job_created = 0, job_finished = 0;
    int provision_requested = 0, key_issued = 0, key_revoked = 0;
    for (const std::shared_ptr<zetta::governance::AuditEvent>& event : driver.audit_events()) {
        switch (event->action()) {
            case zetta::governance::AuditAction::DataAccess: ++data_access; break;
            case zetta::governance::AuditAction::JobCreated: ++job_created; break;
            case zetta::governance::AuditAction::JobFinished: ++job_finished; break;
            case zetta::governance::AuditAction::ProvisionRequested: ++provision_requested; break;
            case zetta::governance::AuditAction::KeyIssued: ++key_issued; break;
            case zetta::governance::AuditAction::KeyRevoked: ++key_revoked; break;
            default: break;
        }
    }
    // The 3 authorize(resource) decisions + the featurize/train/explain/score jobs on
    // the driver + the blocked over-cap burst + the burst identity lifecycle.
    if (data_access < 2 || job_created < 4 || job_finished < 4) {
        return TestResult::fail("the run was not fully audited (data_access=" +
                                std::to_string(data_access) + " job_created=" +
                                std::to_string(job_created) + " job_finished=" +
                                std::to_string(job_finished) + ")");
    }
    if (provision_requested < 1) {
        return TestResult::fail("the blocked over-cap burst left no provision_requested "
                                "audit event");
    }
    if (key_issued < static_cast<int>(minted.size()) ||
        key_revoked < static_cast<int>(revoked)) {
        return TestResult::fail("the burst identity lifecycle was not fully audited "
                                "(key_issued=" + std::to_string(key_issued) +
                                " key_revoked=" + std::to_string(key_revoked) + ")");
    }

    TestResult result = TestResult::pass(
        "loaded the saas parquet as one partitioned table, burst within the cost cap "
        "(over-cap blocked), rebalanced partitions onto the burst nodes, featurized + "
        "trained a calibrated `" + label + "` model compute-near-data across the grown "
        "cluster, then tore the burst down with the model still serving — gated, "
        "governed, and identical on every node");
    result.metric("permanent nodes", std::to_string(permanent))
        .metric("burst nodes", std::to_string(added.members.size()))
        .metric("requested nodes (script)", std::to_string(requested_nodes))
        .metric("cost cap (units)", number(*cost_cap))
        .metric("burst cost (units)", number(burst_cost))
        .metric("over-cap burst blocked", blocked.members.empty() ? "yes" : "no")
        .metric("partitions on burst nodes", std::to_string(partitions_on_burst))
        .metric("partitions drained back", std::to_string(drained.moved))
        .metric("burst identities minted / revoked",
                std::to_string(minted.size()) + " / " + std::to_string(revoked))
        .metric("conversations", std::to_string(dataset.rows.size()))
        .metric("real fixture rows", std::to_string(dataset.real_rows))
        .metric("feature-table rows", std::to_string(results[0].feature_table.row_count()))
        .metric("top churn driver", results[0].train.top_driver)
        .metric("AUC", number(results[0].train.metrics.auc))
        .metric("calibrated", results[0].train.calibrated ? "yes" : "no")
        .metric("raw data read near data (bytes)", std::to_string(total_local_bytes))
        .metric("partials over wire (bytes)", std::to_string(total_wire_bytes))
        .metric("audit events", std::to_string(data_access + job_created + job_finished +
                                               provision_requested + key_issued + key_revoked));
    return result;
}

}  // namespace

const std::string& uc1_at_scale_script() { return uc1_at_scale_source(); }

void register_uc1_scale_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "cluster.uc1_at_scale",
        .category = TestCategory::Distributed,
        .name = "UC-1 at scale (TC-12)",
        .description =
            "The capstone scale test, driven by one canonical sim-Python program: load "
            "the saas-sales-conversations parquet as one partitioned table across the "
            "permanent members of a real replicated loopback cluster, burst within the "
            "script's cost_cap= through the BurstApprovalGate (an over-cap burst is "
            "blocked deny-by-default + audited), rebalance partitions onto the burst "
            "nodes, mint short-lived burst identities, featurize + train a calibrated "
            "GBDT on `outcome` compute-near-data across the grown cluster (explained + "
            "ranked), then tear the burst down — partitions drain back, identities are "
            "revoked, and the model stays live for serving (UC-4). Gated (RULE-1) and "
            "governed; the model is identical on every node with only partials over "
            "the wire (the cloud burst is replaced by a local provisioner over the "
            "standby fleet so it runs headless).",
        .options = {TestOption::integer("nodes", "Permanent nodes", 2, 1, 4),
                    TestOption::integer("burst", "Burst nodes", 1, 1, 3),
                    TestOption::integer("rows", "Conversations", 96, 12, 200000),
                    TestOption::boolean("calibrate", "Calibrate the model", true)},
        .run = run_uc1_at_scale,
    });
}

}  // namespace zetta::testkit
