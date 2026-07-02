#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/input/datasets_bindings.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/data/table.hpp"

// #521 — the simulated HuggingFace `datasets` shim. A sim-Python
//
//   from datasets import load_dataset
//   ds = load_dataset("<path>", columns=[...])
//
// lowers to an IR connect/load node, and run_load_dataset_step (or the
// DatasetStepExecutor wired into a GraphExecutor) executes it via the native
// Parquet reader, producing a (partitioned) table. HuggingFace `datasets` is the
// fidelity reference only — nothing here imports it or touches the network.
namespace {

namespace fs = std::filesystem;

using zetta::Database;
using zetta::GraphExecutor;
using zetta::Node;
using zetta::NodeConfig;
using zetta::RoundRobinPartitioner;
using zetta::RunStatus;
using zetta::Table;
using zetta::input::DatasetLoadResult;
using zetta::input::dataset_load_spec;
using zetta::input::DatasetStepExecutor;
using zetta::input::is_dataset_load_step;
using zetta::input::LoadDatasetOptions;
using zetta::input::PythonFrontend;
using zetta::input::run_load_dataset_step;
using zetta::ir::StepKind;
using zetta::test::TempDir;

fs::path saas_fixture_path() {
    return fs::path(ZETTA_TEST_FIXTURES_DIR) / "parquet" / "saas_sample.parquet";
}

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// Read back all of a table's stored row documents (in storage order).
std::vector<nlohmann::json> table_rows(const Table& table) {
    std::vector<nlohmann::json> rows;
    const auto& store = table.store();
    for (std::size_t i = 0; i < store.size(); ++i) rows.push_back(store.at(i));
    return rows;
}

// Lower a sim-Python snippet and hand back the single load node it produced.
std::shared_ptr<zetta::ir::Node> lower_load(const std::string& source,
                                            const std::string& node_id) {
    const auto lowered = PythonFrontend{}.compile(source);
    EXPECT_TRUE(lowered.ok()) << lowered.error;
    return lowered.graph.find_node(node_id);
}

// =====================================================================
// Lowering: load_dataset(...) → a connect/load node carrying path + projection
// =====================================================================

// `from datasets import load_dataset; load_dataset("…")` lowers to a single
// connect-stage node (the import line produces nothing) that this binding
// recognizes as a Parquet dataset-load step.
TEST(DatasetsShim, LoadDatasetLowersToARecognizedConnectStep) {
    const auto lowered = PythonFrontend{}.compile(
        "from datasets import load_dataset\n"
        "ds = load_dataset('saas_sample.parquet')\n");
    ASSERT_TRUE(lowered.ok()) << lowered.error;

    const auto step = lowered.graph.find_node("load_dataset");
    ASSERT_TRUE(step);
    EXPECT_EQ(step->kind, StepKind::Connect);
    EXPECT_TRUE(is_dataset_load_step(*step));

    const auto spec = dataset_load_spec(*step);
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->source, "saas_sample.parquet");
    EXPECT_TRUE(spec->columns.empty());
}

// The path + the column projection both ride in the lowered node and read back.
TEST(DatasetsShim, SpecReadsPathAndColumnProjection) {
    const auto step = lower_load(
        "ds = load_dataset('saas_sample.parquet', columns=['messages', 'outcome'])\n",
        "load_dataset");
    ASSERT_TRUE(step);

    const auto spec = dataset_load_spec(*step);
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->source, "saas_sample.parquet");
    ASSERT_EQ(spec->columns.size(), 2u);
    EXPECT_EQ(spec->columns[0], "messages");
    EXPECT_EQ(spec->columns[1], "outcome");
}

// The HuggingFace builder-with-data_files shape resolves the source from the
// keyword, not the builder name; `split` rides along too.
TEST(DatasetsShim, SpecHandlesBuilderWithDataFilesAndSplit) {
    const auto step = lower_load(
        "ds = load_dataset('parquet', data_files='shard.parquet', split='train')\n",
        "load_dataset");
    ASSERT_TRUE(step);

    const auto spec = dataset_load_spec(*step);
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->source, "shard.parquet");
    EXPECT_EQ(spec->split, "train");
}

// =====================================================================
// Execution: the binding reads the real Parquet file via the native reader
// =====================================================================

// Acceptance (#521): load_dataset(<path>) yields a table populated from the
// Parquet file through the native reader — every column, every row.
TEST(DatasetsShim, LoadsFullTableViaNativeParquetReader) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const auto step =
        lower_load("ds = load_dataset('" + saas_fixture_path().string() + "')\n",
                   "load_dataset");
    ASSERT_TRUE(step);

    const DatasetLoadResult loaded = run_load_dataset_step(*step, node);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    ASSERT_TRUE(loaded.table);

    // A table named for the dataset, in the default "datasets" database.
    EXPECT_EQ(loaded.database->name(), "datasets");
    EXPECT_EQ(loaded.table->name(), "saas_sample");
    EXPECT_EQ(loaded.rows, 12u);
    EXPECT_EQ(loaded.table->row_count(), 12u);

    // No projection -> every leaf column survives (the engineered columns + the 8
    // embedding_* columns of the fixture).
    const std::vector<nlohmann::json> rows = table_rows(*loaded.table);
    ASSERT_EQ(rows.size(), 12u);
    EXPECT_EQ(rows.front().size(), 13u);
    EXPECT_TRUE(rows.front().contains("messages"));
    EXPECT_TRUE(rows.front().contains("embedding_0"));
    EXPECT_EQ(rows.front()["messages"].get<std::int64_t>(), 12);
    EXPECT_EQ(rows.front()["outcome"].get<std::int64_t>(), 1);
}

// Acceptance (#521): the column projection on the load node is honored — only the
// named columns land in the table, dropping the embeddings without surfacing them.
TEST(DatasetsShim, ColumnProjectionDropsUnselectedColumns) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const auto step = lower_load(
        "ds = load_dataset('" + saas_fixture_path().string() +
            "', columns=['messages', 'outcome'])\n",
        "load_dataset");
    ASSERT_TRUE(step);

    const DatasetLoadResult loaded = run_load_dataset_step(*step, node);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    EXPECT_EQ(loaded.rows, 12u);

    const std::vector<nlohmann::json> rows = table_rows(*loaded.table);
    ASSERT_EQ(rows.size(), 12u);
    for (const nlohmann::json& row : rows) {
        EXPECT_EQ(row.size(), 2u);
        EXPECT_TRUE(row.contains("messages"));
        EXPECT_TRUE(row.contains("outcome"));
        for (auto it = row.begin(); it != row.end(); ++it) {
            EXPECT_NE(it.key().rfind("embedding_", 0), 0u)
                << "embedding column leaked: " << it.key();
        }
    }
    EXPECT_EQ(rows.front()["messages"].get<std::int64_t>(), 12);
}

// Acceptance (#521): the load can produce a *partitioned* table — when a
// partitioner is configured, the populated table is written through the
// partitioned write path (blobs + a catalog entry per partition).
TEST(DatasetsShim, ProducesAPartitionedTable) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const auto step =
        lower_load("ds = load_dataset('" + saas_fixture_path().string() + "')\n",
                   "load_dataset");
    ASSERT_TRUE(step);

    LoadDatasetOptions options;
    options.partitioner = std::make_shared<RoundRobinPartitioner>(4);

    const DatasetLoadResult loaded = run_load_dataset_step(*step, node, {}, options);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    EXPECT_EQ(loaded.rows, 12u);

    // 12 rows round-robin over P=4 -> 4 non-empty partitions of 3 rows each.
    ASSERT_EQ(loaded.partitions.size(), 4u);
    std::uint64_t total = 0;
    for (const auto& partition : loaded.partitions) {
        ASSERT_TRUE(partition);
        EXPECT_EQ(partition->table(), loaded.table->id());
        total += partition->row_range().end - partition->row_range().start;
    }
    EXPECT_EQ(total, 12u);
    // The catalog is globally visible (system.dataset_partitions).
    EXPECT_EQ(node.dataset_partitions().size(), 4u);
}

// =====================================================================
// The DatasetStepExecutor runs a lowered load node through the GraphExecutor.
// =====================================================================

// The headline acceptance: a load_dataset(...) graph executes through the
// GraphExecutor via the native Parquet reader, producing a Run whose dataset
// output references the real, populated table.
TEST(DatasetsShim, GraphExecutorRunsTheLoadStepEndToEnd) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const auto lowered = PythonFrontend{}.compile(
        "from datasets import load_dataset\n"
        "ds = load_dataset('" + saas_fixture_path().string() +
        "', columns=['messages', 'outcome'])\n");
    ASSERT_TRUE(lowered.ok()) << lowered.error;

    auto executor = std::make_shared<DatasetStepExecutor>(node);
    GraphExecutor runner(executor);
    const zetta::ExecutionResult result = runner.execute(node, lowered.graph);

    ASSERT_TRUE(result.run);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.run->status(), RunStatus::Succeeded);

    // The connect step emitted a dataset output pointing at the real table.
    ASSERT_EQ(result.run->outputs().size(), 1u);
    const zetta::RunOutput& output = result.run->outputs().front();
    EXPECT_EQ(output.kind, "dataset");
    EXPECT_EQ(output.ref.rfind("tbl:", 0), 0u);

    // And the table actually exists with the projected columns.
    const auto table = node.find_database("datasets")->find_table("saas_sample");
    ASSERT_TRUE(table);
    EXPECT_EQ(output.ref, "tbl:" + table->id());
    EXPECT_EQ(table->row_count(), 12u);
}

// =====================================================================
// Error paths — non-throwing, clear reasons.
// =====================================================================

// An unresolvable source (a HuggingFace name the offline default cannot fetch)
// fails with a clear reason and writes nothing.
TEST(DatasetsShim, UnresolvableSourceFailsCleanly) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const auto step = lower_load(
        "ds = load_dataset('DeepMostInnovations/saas-sales-conversations')\n",
        "load_dataset");
    ASSERT_TRUE(step);

    const DatasetLoadResult loaded = run_load_dataset_step(*step, node);
    EXPECT_FALSE(loaded.ok());
    EXPECT_NE(loaded.error.find("not a readable local file"), std::string::npos)
        << loaded.error;
    EXPECT_EQ(node.database_count(), 0u);  // nothing created
}

// A projection naming a column that is not in the dataset fails with a reason.
TEST(DatasetsShim, ProjectionOfAbsentColumnFails) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const auto step = lower_load(
        "ds = load_dataset('" + saas_fixture_path().string() +
            "', columns=['messages', 'does_not_exist'])\n",
        "load_dataset");
    ASSERT_TRUE(step);

    const DatasetLoadResult loaded = run_load_dataset_step(*step, node);
    EXPECT_FALSE(loaded.ok());
    EXPECT_NE(loaded.error.find("does_not_exist"), std::string::npos)
        << loaded.error;
}

// An injected resolver lets a consenting host supply bytes by any means (proving
// the IO seam) without the engine touching the network.
TEST(DatasetsShim, InjectedResolverSuppliesBytes) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    std::string captured_source;
    const auto resolver =
        [&](const std::string& source) -> zetta::input::DatasetResolveResult {
        captured_source = source;
        std::ifstream stream(saas_fixture_path(), std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return {buffer.str(), ""};
    };

    const auto step = lower_load(
        "ds = load_dataset('hf://some/remote/name')\n", "load_dataset");
    ASSERT_TRUE(step);

    const DatasetLoadResult loaded = run_load_dataset_step(*step, node, resolver);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    EXPECT_EQ(captured_source, "hf://some/remote/name");
    EXPECT_EQ(loaded.rows, 12u);
}

}  // namespace
