#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../base64.hpp"  // zetta::detail::base64_decode_to_string
#include "cases.hpp"
#include "parquet_fixture_data.hpp"
#include "zetta/access_key.hpp"
#include "zetta/add_nodes.hpp"
#include "zetta/audit_event.hpp"
#include "zetta/capability.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/database.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/gate.hpp"
#include "zetta/input/cluster_bindings.hpp"
#include "zetta/input/datasets_bindings.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/local_provisioner.hpp"
#include "zetta/ml/calibration.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/local_train_job.hpp"
#include "zetta/model.hpp"
#include "zetta/node.hpp"
#include "zetta/node_capability.hpp"
#include "zetta/parquet/reader.hpp"
#include "zetta/partition_writer.hpp"
#include "zetta/partitioner.hpp"
#include "zetta/principal.hpp"
#include "zetta/provisioner.hpp"
#include "zetta/scope.hpp"
#include "zetta/table.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/validation.hpp"

// The UC-1-at-scale end-to-end case (TC-12, #531/#502): the capstone scale test.
// One canonical sim-Python program drives the whole distributed pipeline over a
// real (manually-sized) loopback cluster:
//
//   load the saas-sales-conversations parquet as a PARTITIONED table
//     -> add permanent nodes
//       -> train a calibrated GBDT on `outcome` compute-near-data over the partitions
//         -> explain (per-row TreeSHAP drivers) + rank (the at-risk list)
//
// gated (RULE-1) and governed (key-scoped access + an audit trail). The same script
// is registered as a sim-Python example (examples.uc1_at_scale) so it doubles as the
// canonical usage example, and it is the program an operator runs for real in the
// headed app (start a node, add cloud nodes behind the consent gate, launch the case)
// — here the cloud provisioner is replaced by an injected LocalProvisioner so the
// case is hermetic and runs headless. See docs/uc1-at-scale-e2e.md.
namespace zetta::testkit {
namespace {

using Json = nlohmann::json;
using compute::CpuDevice;
using compute::DataType;
using ddf::ColumnSchema;
using ddf::DataFrameSchema;
using ml::NearDataTrainResult;

// The canonical UC-1-at-scale program. One statement per line (the sim-Python
// frontend lowers each call to a typed step; refs become data-flow edges):
//   cluster.add_nodes -> act       load_dataset -> connect
//   train -> model                 explain -> serve            rank -> index
const std::string& uc1_at_scale_source() {
    static const std::string source =
        "nodes = cluster.add_nodes(10, provider='azure')\n"
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

// One member of the running cluster: the device + coordinator + table id the SPMD
// near-data train runs against (the Node + Cluster are owned by the LoopbackCluster).
struct Member {
    std::unique_ptr<CpuDevice> device;
    std::unique_ptr<compute::JobCoordinator> coordinator;
    std::string table_id;
};

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

std::size_t node_count_option(const TestOptions& options) {
    return static_cast<std::size_t>(
        std::min<long>(6, std::max<long>(1, options.get_int("nodes", 3))));
}

TestResult run_uc1_at_scale(const TestOptions& options, TestContext& context) {
    const std::size_t nodes = node_count_option(options);
    const std::size_t rows = static_cast<std::size_t>(std::max<long>(
        static_cast<long>(std::max<std::size_t>(nodes, 12)),
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
    const ValidationReport gate = default_gate().run(graph);
    if (!gate.passed() || gate.block_deploy) {
        std::string reason = "gate blocked deploy";
        for (const CheckResult& check : gate.checks) {
            if (check.status == CheckStatus::Fail) reason = check.name + ": " + check.reason;
        }
        return TestResult::fail("the correctness gate blocked the pipeline: " + reason);
    }
    context.emit("gate passed (" + std::to_string(gate.checks.size()) + " checks)");

    // ---- 3) Read the data shape + the requested node count FROM the script ------
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

    // ---- 5) Stand up the (manually-sized) cluster ------------------------------
    context.emit("bringing up a " + std::to_string(nodes) + "-node loopback cluster");
    LoopbackCluster cluster(nodes);
    if (!cluster.connected()) return TestResult::fail("cluster did not converge");

    // Write each node's round-robin shard into the partitions it owns (the
    // compute-near-data layout: each node holds its own slice as a partitioned table).
    std::vector<Member> members(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        members[i].device = std::make_unique<CpuDevice>();
        Node& node = cluster.node(i);
        std::shared_ptr<Database> database = node.create_database("data");
        if (!database) return TestResult::fail("create_database failed on node " + std::to_string(i));
        std::shared_ptr<Table> table = database->create_table("conversations");
        if (!table) return TestResult::fail("create_table failed on node " + std::to_string(i));
        table->set_partitioner(std::make_shared<RoundRobinPartitioner>(4));
        for (std::size_t r = 0; r < dataset.rows.size(); ++r) {
            if (r % nodes == i) table->add_row(dataset.rows[r]);
        }
        const PartitionWriteResult written = write_partitioned(node, *database, *table);
        if (!written.ok()) {
            return TestResult::fail("write_partitioned failed on node " + std::to_string(i) +
                                    ": " + written.error);
        }
        members[i].table_id = table->id();
    }

    // ---- 6) Add permanent nodes — the lowered cluster.add_nodes(10, provider=) --
    // The provider= is provenance only; the Provisioner is INJECTED (cloud stays
    // consent-gated). Here a LocalProvisioner over the live cluster fleet stands in for
    // the Azure burst, so the control path runs hermetically.
    LocalProvisioner provisioner([&cluster]() {
        std::vector<ProvisionedNode> fleet;
        for (std::size_t i = 0; i < cluster.size(); ++i) {
            fleet.push_back(ProvisionedNode{.node_id = cluster.node_id(i),
                                            .address = cluster.cluster(i).listen_address(),
                                            .capability = NodeCapability{}});
        }
        return fleet;
    });
    const AddNodesResult added = input::run_add_nodes_step(*add_step, provisioner);
    if (added.members.empty()) {
        return TestResult::fail("cluster.add_nodes provisioned no nodes");
    }
    context.emit("cluster.add_nodes(" + std::to_string(requested_nodes) + ") -> " +
                 std::to_string(added.members.size()) + " members" +
                 (added.satisfied ? " (satisfied)" : " (best-effort on given hardware)"));

    // ---- 7) Governance: a scoped key authorizes the run; the trail is audited ---
    Node& driver = cluster.node(0);
    std::shared_ptr<Principal> principal =
        driver.create_principal("uc1-pipeline", PrincipalKind::Service);
    if (!principal) return TestResult::fail("create_principal failed");
    Scope scope;
    scope.grant(ResourceKind::Project, "uc1").grant(ResourceKind::Database, "data");
    CapabilitySet capabilities;
    capabilities.grant(CapabilityDomain::Orchestration, "start_run")
        .grant(CapabilityDomain::Data, "read");
    std::shared_ptr<AccessKey> key = principal->create_key(scope, capabilities);
    if (!key) return TestResult::fail("create_key failed");
    // The key may start the run + read the project's data; it may not reach elsewhere.
    if (!driver.authorize(key->id(), Operation{CapabilityDomain::Orchestration, "start_run"})) {
        return TestResult::fail("the scoped key was denied orchestration:start_run");
    }
    if (!driver.authorize(key->id(), Resource{ResourceKind::Database, "data"})) {
        return TestResult::fail("the scoped key was denied access to its own database");
    }
    if (driver.authorize(key->id(), Resource{ResourceKind::Project, "unrelated"})) {
        return TestResult::fail("the scoped key reached a project outside its scope");
    }

    // ---- 8) SPMD near-data train -> explain -> rank across the cluster ----------
    context.emit("training on '" + label + "' compute-near-data over the partitions");
    std::vector<std::shared_ptr<ComputeJob>> jobs(nodes);
    std::vector<NearDataTrainResult> results(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        members[i].coordinator =
            std::make_unique<compute::JobCoordinator>(cluster.cluster(i), /*workers=*/2);
        members[i].coordinator->start();
    }
    {
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < nodes; ++i) {
            jobs[i] = cluster.node(i).create_job(JobKind::Train);
            threads.emplace_back([&, i] {
                results[i] = ml::run_near_data_train_job(
                    *members[i].coordinator, cluster.node(i), *jobs[i], members[i].table_id,
                    schema, feature_columns, label, gbdt_params(), *members[i].device,
                    "conversation_id", calibration_config(calibrate), "uc1-outcome-gbdt",
                    /*top_k=*/0, /*round_tag=*/"uc1.at_scale", std::chrono::seconds(20));
            });
        }
        for (std::thread& thread : threads) thread.join();
    }

    // ---- 9) Assert the at-scale result -----------------------------------------
    std::uint64_t total_rows_read = 0;
    std::uint64_t total_local_bytes = 0;
    std::uint64_t total_wire_bytes = 0;
    std::size_t total_explanations = 0;
    for (std::size_t i = 0; i < nodes; ++i) {
        const NearDataTrainResult& result = results[i];
        if (result.status != JobStatus::Succeeded) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " near-data train did not succeed: " + jobs[i]->detail());
        }
        // The pipeline produced a model, per-row drivers, and a ranked at-risk list.
        if (result.feature_names != feature_columns) {
            return TestResult::fail("node " + std::to_string(i) + " feature ordering diverged");
        }
        if (result.top_driver.empty()) {
            return TestResult::fail("node " + std::to_string(i) + " surfaced no churn driver");
        }
        if (result.ranked.size() != dataset.rows.size()) {
            return TestResult::fail("node " + std::to_string(i) + " ranked " +
                                    std::to_string(result.ranked.size()) + " of " +
                                    std::to_string(dataset.rows.size()) + " conversations");
        }
        for (std::size_t r = 1; r < result.ranked.size(); ++r) {
            if (result.ranked[r - 1].risk < result.ranked[r].risk) {
                return TestResult::fail("node " + std::to_string(i) +
                                        " at-risk list is not ranked by risk descending");
            }
        }
        for (const ml::EntityScore& score : result.ranked) {
            if (score.risk < 0.0 || score.risk > 1.0) {
                return TestResult::fail("node " + std::to_string(i) + " risk out of [0,1]");
            }
        }
        // Every node converges on the IDENTICAL model + metrics + ranking (SPMD broadcast).
        if (result.model.to_json().dump() != results[0].model.to_json().dump()) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " model differs from the root (single-node != multi-node)");
        }
        if (result.ranked.size() != results[0].ranked.size()) {
            return TestResult::fail("node " + std::to_string(i) + " ranking differs from the root");
        }
        // Near-data instrumentation: this node read its OWN partitions off local disk
        // (the raw data never crossed the wire — only the training partials do), and on
        // a multi-node cluster it read only its shard, not the whole dataset.
        if (result.partitions_read == 0 || result.local_bytes_read == 0) {
            return TestResult::fail("node " + std::to_string(i) + " read no owned partitions");
        }
        if (result.wire_bytes_moved == 0) {
            return TestResult::fail("node " + std::to_string(i) + " exchanged no partials");
        }
        if (nodes > 1 && result.rows_read >= dataset.rows.size()) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " read the whole dataset instead of its shard");
        }
        total_rows_read += result.rows_read;
        total_local_bytes += result.local_bytes_read;
        total_wire_bytes += result.wire_bytes_moved;
        total_explanations += result.explanations.size();
    }
    // The model learned a real signal off the (mostly) separable saas data.
    if (!(results[0].metrics.auc > 0.6)) {
        return TestResult::fail("the trained model did not learn the signal (AUC " +
                                number(results[0].metrics.auc) + ")");
    }
    if (calibrate && !results[0].calibrated) {
        return TestResult::fail("calibration was requested but the model was not calibrated");
    }
    // Every conversation was read near data, across the shards, and explained exactly once.
    if (total_rows_read != dataset.rows.size() || total_explanations != dataset.rows.size()) {
        return TestResult::fail("rows read near data (" + std::to_string(total_rows_read) +
                                ") / explained (" + std::to_string(total_explanations) +
                                ") != conversations (" + std::to_string(dataset.rows.size()) + ")");
    }
    // The produced Model is registered on the deterministic root (system.zetta_models);
    // it replicates to peers. The root is the lowest node id — not necessarily node 0.
    std::size_t root_index = 0;
    bool found_root = false;
    for (std::size_t i = 0; i < nodes; ++i) {
        if (results[i].is_root) {
            root_index = i;
            found_root = true;
            break;
        }
    }
    if (!found_root) return TestResult::fail("no node reported being the collective root");
    if (results[root_index].model_id.empty() ||
        !cluster.node(root_index).find_model(results[root_index].model_id)) {
        return TestResult::fail("the trained Model was not registered on the root node");
    }

    // ---- 10) Governance: the run was audited -----------------------------------
    int data_access = 0, job_created = 0, job_finished = 0;
    for (const std::shared_ptr<AuditEvent>& event : driver.audit_events()) {
        switch (event->action()) {
            case AuditAction::DataAccess: ++data_access; break;
            case AuditAction::JobCreated: ++job_created; break;
            case AuditAction::JobFinished: ++job_finished; break;
            default: break;
        }
    }
    // The 3 authorize(resource) decisions + the train/explain/score jobs on the driver.
    if (data_access < 2 || job_created < 3 || job_finished < 3) {
        return TestResult::fail("the run was not fully audited (data_access=" +
                                std::to_string(data_access) + " job_created=" +
                                std::to_string(job_created) + " job_finished=" +
                                std::to_string(job_finished) + ")");
    }

    TestResult result = TestResult::pass(
        "loaded the saas parquet partitioned, added nodes, and trained a calibrated "
        "`" + label + "` model near data across the cluster — gated, governed, and "
        "identical on every node");
    result.metric("nodes", std::to_string(nodes))
        .metric("requested nodes (script)", std::to_string(requested_nodes))
        .metric("provisioned", std::to_string(added.members.size()))
        .metric("conversations", std::to_string(dataset.rows.size()))
        .metric("real fixture rows", std::to_string(dataset.real_rows))
        .metric("top churn driver", results[0].top_driver)
        .metric("AUC", number(results[0].metrics.auc))
        .metric("calibrated", results[0].calibrated ? "yes" : "no")
        .metric("raw data read near data (bytes)", std::to_string(total_local_bytes))
        .metric("partials over wire (bytes)", std::to_string(total_wire_bytes))
        .metric("audit events", std::to_string(data_access + job_created + job_finished));
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
            "the saas-sales-conversations parquet as a partitioned table, add permanent "
            "nodes, and train a calibrated GBDT on `outcome` compute-near-data across a "
            "real loopback cluster — explained (per-row TreeSHAP drivers) and ranked "
            "(the at-risk list), gated (RULE-1) and governed (key-scoped access + audit). "
            "Asserts a real model identical on every node, with only partials over the "
            "wire (the cloud burst is replaced by a local provisioner so it runs headless).",
        .options = {TestOption::integer("nodes", "Nodes", 3, 1, 6),
                    TestOption::integer("rows", "Conversations", 96, 12, 200000),
                    TestOption::boolean("calibrate", "Calibrate the model", true)},
        .run = run_uc1_at_scale,
    });
}

}  // namespace zetta::testkit
