#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/node.hpp"
#include "zetta/data/table.hpp"

// #587 — the "Add dataset" engine backend behind the headed affordance. Read a
// local parquet / jsonl / json / csv file and materialize it as a *partitioned*
// table on the node, so it surfaces in system.datasets with its partitions placed
// across the cluster. zetta-core, headless-testable (no SDL); nothing here touches
// the network.
namespace {

namespace fs = std::filesystem;

using zetta::AddDatasetOptions;
using zetta::AddDatasetResult;
using zetta::add_dataset_file;
using zetta::Database;
using zetta::DatasetSummary;
using zetta::Node;
using zetta::NodeConfig;
using zetta::Table;
using zetta::dataset_file::Format;
using zetta::test::TempDir;

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

fs::path saas_fixture_path() {
    return fs::path(ZETTA_TEST_FIXTURES_DIR) / "parquet" / "saas_sample.parquet";
}

// Write `content` to a fresh file named `name` under `dir`, returning its path.
fs::path write_file(const fs::path& dir, const std::string& name,
                    const std::string& content) {
    const fs::path path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
    return path;
}

// Read back all of a table's stored row documents (in storage order).
std::vector<nlohmann::json> table_rows(const Table& table) {
    std::vector<nlohmann::json> rows;
    const auto& store = table.store();
    for (std::size_t i = 0; i < store.size(); ++i) rows.push_back(store.at(i));
    return rows;
}

// The dataset summary for a table id, or nullptr.
const DatasetSummary* find_dataset(const std::vector<DatasetSummary>& datasets,
                                   const std::string& table_id) {
    for (const DatasetSummary& dataset : datasets) {
        if (dataset.table_id == table_id) return &dataset;
    }
    return nullptr;
}

// =====================================================================
// Format classification
// =====================================================================

TEST(AddDataset, ClassifiesFormatsByExtension) {
    using zetta::dataset_file::format_from_path;
    EXPECT_EQ(format_from_path("a/b/data.parquet"), Format::Parquet);
    EXPECT_EQ(format_from_path("DATA.PQ"), Format::Parquet);
    EXPECT_EQ(format_from_path("events.jsonl"), Format::Jsonl);
    EXPECT_EQ(format_from_path("events.ndjson"), Format::Jsonl);
    EXPECT_EQ(format_from_path("blob.json"), Format::Json);
    EXPECT_EQ(format_from_path("table.csv"), Format::Csv);
    EXPECT_EQ(format_from_path("notes.txt"), Format::Unknown);
    EXPECT_EQ(format_from_path("noext"), Format::Unknown);
}

TEST(AddDataset, DialogExtensionsCoverEveryFormat) {
    const std::string extensions = zetta::dataset_file::dialog_extensions();
    for (const char* ext : {"parquet", "pq", "jsonl", "ndjson", "json", "csv"}) {
        EXPECT_NE(extensions.find(ext), std::string::npos) << ext;
    }
}

// =====================================================================
// CSV
// =====================================================================

TEST(AddDataset, LoadsCsvAsPartitionedTable) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const fs::path csv = write_file(tmp.path(), "accounts.csv",
                                    "name,arr,active\n"
                                    "globex,1200,true\n"
                                    "initech,3400,false\n"
                                    "acme,560,true\n");

    const AddDatasetResult result = add_dataset_file(node, csv);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.format, Format::Csv);
    EXPECT_EQ(result.rows, 3u);
    ASSERT_TRUE(result.table);
    EXPECT_EQ(result.table->name(), "accounts");
    EXPECT_EQ(result.database->name(), "datasets");

    // It is partitioned: one catalog entry per non-empty partition, each placed.
    ASSERT_FALSE(result.partitions.empty());
    for (const auto& partition : result.partitions) {
        EXPECT_FALSE(partition->owner_nodes().empty());
    }

    // CSV cells are type-inferred: a number stays a number, a bool a bool.
    const std::vector<nlohmann::json> rows = table_rows(*result.table);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0]["name"], "globex");
    EXPECT_EQ(rows[0]["arr"], 1200);
    EXPECT_TRUE(rows[0]["arr"].is_number_integer());
    EXPECT_EQ(rows[0]["active"], true);
    EXPECT_EQ(rows[1]["active"], false);
}

TEST(AddDataset, CsvTypeInferenceKeepsLeadingZerosAsStrings) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const fs::path csv = write_file(tmp.path(), "codes.csv",
                                    "zip,ratio,label,blank\n"
                                    "01234,3.14,hello,\n"
                                    "90210,-2.5,\"a,b\",\n");

    const AddDatasetResult result = add_dataset_file(node, csv);
    ASSERT_TRUE(result.ok()) << result.error;
    const std::vector<nlohmann::json> rows = table_rows(*result.table);
    ASSERT_EQ(rows.size(), 2u);

    // A multi-digit value with a leading zero is NOT silently re-numbered.
    EXPECT_TRUE(rows[0]["zip"].is_string());
    EXPECT_EQ(rows[0]["zip"], "01234");
    EXPECT_EQ(rows[1]["zip"], 90210);  // no leading zero → integer
    EXPECT_TRUE(rows[0]["ratio"].is_number_float());
    EXPECT_DOUBLE_EQ(rows[1]["ratio"].get<double>(), -2.5);
    // A quoted field with an embedded comma is one cell.
    EXPECT_EQ(rows[1]["label"], "a,b");
    // An empty cell is null.
    EXPECT_TRUE(rows[0]["blank"].is_null());
}

TEST(AddDataset, RejectsCsvWithNoHeader) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const fs::path csv = write_file(tmp.path(), "empty.csv", "\n\n");
    const AddDatasetResult result = add_dataset_file(node, csv);
    EXPECT_FALSE(result.ok());
}

// =====================================================================
// JSONL / JSON
// =====================================================================

TEST(AddDataset, LoadsJsonlAsPartitionedTable) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const fs::path jsonl =
        write_file(tmp.path(), "events.jsonl",
                   "{\"id\": 1, \"kind\": \"login\"}\n"
                   "\n"  // blank line skipped
                   "{\"id\": 2, \"kind\": \"logout\"}\n");

    const AddDatasetResult result = add_dataset_file(node, jsonl);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.format, Format::Jsonl);
    EXPECT_EQ(result.rows, 2u);
    ASSERT_FALSE(result.partitions.empty());

    const std::vector<nlohmann::json> rows = table_rows(*result.table);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]["kind"], "login");
}

TEST(AddDataset, RejectsMalformedJsonl) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const fs::path jsonl = write_file(tmp.path(), "bad.jsonl",
                                      "{\"ok\": 1}\n"
                                      "{not valid json}\n");
    const AddDatasetResult result = add_dataset_file(node, jsonl);
    EXPECT_FALSE(result.ok());
    // Nothing should have been left half-loaded.
    EXPECT_EQ(result.rows, 0u);
}

TEST(AddDataset, LoadsJsonArrayAsRows) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const fs::path json = write_file(tmp.path(), "people.json",
                                     "[{\"name\": \"ana\"}, {\"name\": \"bo\"}]");
    const AddDatasetResult result = add_dataset_file(node, json);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.format, Format::Json);
    EXPECT_EQ(result.rows, 2u);
}

// =====================================================================
// Parquet
// =====================================================================

TEST(AddDataset, LoadsParquetFixtureAsPartitionedTable) {
    if (!fs::exists(saas_fixture_path())) GTEST_SKIP() << "parquet fixture missing";
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const AddDatasetResult result = add_dataset_file(node, saas_fixture_path());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.format, Format::Parquet);
    EXPECT_GT(result.rows, 0u);
    EXPECT_EQ(result.table->name(), "saas_sample");
    ASSERT_FALSE(result.partitions.empty());
}

// =====================================================================
// Placement across the cluster + system.datasets
// =====================================================================

TEST(AddDataset, PlacesPartitionsAcrossMultipleMembers) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    // Twelve distinct rows over an explicit three-node membership: rendezvous
    // placement spreads the partitions across more than one owner.
    std::string jsonl;
    for (int i = 0; i < 12; ++i) {
        jsonl += "{\"id\": " + std::to_string(i) + "}\n";
    }
    const fs::path file = write_file(tmp.path(), "spread.jsonl", jsonl);

    AddDatasetOptions options;
    options.members = {"node-a", "node-b", "node-c"};
    const AddDatasetResult result = add_dataset_file(node, file, options);
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_FALSE(result.partitions.empty());

    std::set<std::string> owners;
    for (const auto& partition : result.partitions) {
        for (const std::string& owner : partition->owner_nodes()) owners.insert(owner);
    }
    EXPECT_GT(owners.size(), 1u) << "partitions should spread across the cluster";
}

TEST(AddDataset, NewDatasetAppearsInSystemDatasets) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const fs::path csv =
        write_file(tmp.path(), "metrics.csv", "a,b\n1,2\n3,4\n5,6\n");
    const AddDatasetResult result = add_dataset_file(node, csv);
    ASSERT_TRUE(result.ok()) << result.error;

    const std::vector<DatasetSummary> datasets = node.datasets();
    const DatasetSummary* summary =
        find_dataset(datasets, result.table->id());
    ASSERT_NE(summary, nullptr) << "the new dataset is not in system.datasets";
    EXPECT_EQ(summary->name, "metrics");
    EXPECT_EQ(summary->rows, 3u);
    EXPECT_FALSE(summary->partitions.empty())
        << "the dataset should show partition placement";
    // The inferred schema carries both columns.
    EXPECT_EQ(summary->columns.size(), 2u);
}

TEST(AddDataset, CanLoadUnpartitioned) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const fs::path csv = write_file(tmp.path(), "plain.csv", "x\n1\n2\n");

    AddDatasetOptions options;
    options.partitioned = false;
    const AddDatasetResult result = add_dataset_file(node, csv, options);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows, 2u);
    EXPECT_TRUE(result.partitions.empty());
}

// =====================================================================
// Errors + naming
// =====================================================================

TEST(AddDataset, RejectsUnsupportedExtension) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const fs::path txt = write_file(tmp.path(), "notes.txt", "hello");
    const AddDatasetResult result = add_dataset_file(node, txt);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.format, Format::Unknown);
}

TEST(AddDataset, RejectsMissingFile) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const AddDatasetResult result =
        add_dataset_file(node, tmp.path() / "does_not_exist.csv");
    EXPECT_FALSE(result.ok());
}

TEST(AddDataset, RejectsEmptyFile) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const fs::path empty = write_file(tmp.path(), "empty.json", "");
    const AddDatasetResult result = add_dataset_file(node, empty);
    EXPECT_FALSE(result.ok());
}

TEST(AddDataset, SecondLoadOfSameFileGetsUniqueName) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const fs::path csv = write_file(tmp.path(), "dupe.csv", "a\n1\n");
    const AddDatasetResult first = add_dataset_file(node, csv);
    const AddDatasetResult second = add_dataset_file(node, csv);
    ASSERT_TRUE(first.ok()) << first.error;
    ASSERT_TRUE(second.ok()) << second.error;
    EXPECT_EQ(first.table->name(), "dupe");
    EXPECT_EQ(second.table->name(), "dupe_2");
}

}  // namespace
