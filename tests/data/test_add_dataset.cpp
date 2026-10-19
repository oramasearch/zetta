#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/node.hpp"
#include "zetta/data/table.hpp"
#include "zetta/parquet/metadata.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/uuid.hpp"

// #587 — the "Add dataset" engine backend behind the headed affordance. Read a
// local parquet / jsonl / json / csv file and materialize it as a *partitioned*
// table on the node, so it surfaces in system.datasets with its partitions placed
// across the cluster. zetta-core, headless-testable (no SDL); nothing here touches
// the network.
namespace {

namespace fs = std::filesystem;

using zetta::data::AddDatasetOptions;
using zetta::data::AddDatasetResult;
using zetta::data::add_dataset_file;
using zetta::data::add_dataset_rows;
using zetta::data::Database;
using zetta::data::DatasetSummary;
using zetta::Node;
using zetta::NodeConfig;
using zetta::data::Table;
using zetta::data::dataset_file::Format;
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
    for (std::size_t i = 0; i < store.size(); ++i) rows.push_back(*store.at(i));
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
    using zetta::data::dataset_file::format_from_path;
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
    const std::string extensions = zetta::data::dataset_file::dialog_extensions();
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

// #1221: a ragged CSV is refused with its line, never padded with nulls or cut
// short — the same rule and words lake applies to an upload (lake PR #459).
TEST(AddDataset, RefusesRaggedCsvNamingTheLine) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const auto refused = [&](const std::string& name, const std::string& content) {
        const AddDatasetResult result =
            add_dataset_file(node, write_file(tmp.path(), name, content));
        EXPECT_FALSE(result.ok()) << name;
        return result.error;
    };
    EXPECT_EQ(refused("short.csv", "a,b\n1,2\n3\n"),
              "line 3: 1 fields, but the header has 2 columns");
    EXPECT_EQ(refused("long.csv", "\na,b\n1,2,3\n"),
              "line 3: 3 fields, but the header has 2 columns");
    EXPECT_EQ(refused("unnamed.csv", "a,,c\n1,2,3\n"),
              "line 1: header column 2 has no name");
    EXPECT_EQ(refused("twice.csv", "a,b,a\n1,2,3\n"),
              "line 1: the header names column 'a' twice");
    EXPECT_EQ(refused("open.csv", "a,b\n\"1,2\n"),
              "line 2: a quoted field is not closed (a row must stay on one line)");
    EXPECT_EQ(refused("blank.csv", "\r\n\r\n"), "the file has no header row");
    EXPECT_FALSE(node.find_database("datasets"));

    // The pure check names the same problems without decoding a row.
    EXPECT_EQ(zetta::data::csv_problem("a,b\n1,2\n3\n"),
              "line 3: 1 fields, but the header has 2 columns");
    EXPECT_EQ(zetta::data::csv_problem("a,b\r\n1,\"x, y\"\r\n\r\n"), "");
}

// #1221: the dataset and database names SQL can query unquoted, as lake checks
// them, in lake's words.
TEST(AddDataset, DatasetNameProblemMatchesLake) {
    using zetta::data::dataset_name_problem;
    EXPECT_EQ(dataset_name_problem("accounts"), "");
    EXPECT_EQ(dataset_name_problem("_Accounts_2026"), "");
    EXPECT_EQ(dataset_name_problem(std::string(63, 'a')), "");
    EXPECT_EQ(dataset_name_problem(""), "a name is required");
    EXPECT_EQ(dataset_name_problem(std::string(64, 'a')), "a name is at most 63 characters");
    EXPECT_EQ(dataset_name_problem("2026_accounts"),
              "'2026_accounts' must start with a letter or an underscore");
    EXPECT_EQ(dataset_name_problem("my.table"),
              "'my.table' can hold only letters, digits and underscores (not '.'), so SQL "
              "can query it");
    EXPECT_EQ(dataset_name_problem("ORDER"), "'ORDER' is a SQL keyword; choose another name");
}

// #1221: a table records what its rows were loaded from — the file or upload
// format, the connector type, or `rows` — separately from the storage format,
// in table.json and the `tbl:` value, so it survives a restart and reaches
// every node.
TEST(AddDataset, RecordsTheSourceFormatOnTheTable) {
    TempDir tmp;
    std::string csv_table;
    {
        Node node(config_for(tmp));
        node.start();
        const AddDatasetResult csv = add_dataset_file(
            node, write_file(tmp.path(), "accounts.csv", "a,b\n1,2\n"));
        ASSERT_TRUE(csv.ok()) << csv.error;
        EXPECT_EQ(csv.source_format, "csv");
        csv_table = csv.table->id();

        const AddDatasetResult jsonl = zetta::data::add_dataset_content(
            node, "{\"a\":1}\n", Format::Jsonl, {.table_name = "events"});
        ASSERT_TRUE(jsonl.ok()) << jsonl.error;
        EXPECT_EQ(jsonl.table->source_format(), "jsonl");

        AddDatasetOptions pulled;
        pulled.table_name = "orders";
        pulled.source_format = "postgres";
        const AddDatasetResult connector =
            add_dataset_rows(node, std::vector<nlohmann::json>{{{"id", 1}}, {{"id", 2}}},
                             pulled);
        ASSERT_TRUE(connector.ok()) << connector.error;
        EXPECT_EQ(connector.table->source_format(), "postgres");
        EXPECT_EQ(connector.format, Format::Jsonl);  // the rows' shape is unchanged
        // The audit names the connector, not the jsonl its rows are shaped as.
        bool audited = false;
        for (const auto& event : node.audit_events()) {
            if (event && event->target() == connector.table->id()) {
                audited = event->detail().ends_with(", postgres");
            }
        }
        EXPECT_TRUE(audited);

        const AddDatasetResult rows = add_dataset_rows(node, std::vector<nlohmann::json>{{{"id", 1}}}, {});
        ASSERT_TRUE(rows.ok()) << rows.error;
        EXPECT_EQ(rows.table->source_format(), "rows");
        const auto plain = node.create_database("plain")->create_table("by_sql");
        ASSERT_TRUE(plain);
        EXPECT_EQ(plain->source_format(), "");

        const std::vector<DatasetSummary> datasets = node.datasets();
        const DatasetSummary* summary = find_dataset(datasets, csv_table);
        ASSERT_NE(summary, nullptr);
        EXPECT_EQ(summary->source_format, "csv");
        EXPECT_EQ(summary->format, "jsonl");

        if (node.replication()) {
            const nlohmann::json document = node.replication()->snapshot();
            bool replicated = false;
            for (const auto& [key, value] : document.items()) {
                if (key.ends_with(connector.table->id()) && value.is_object() &&
                    value.value("source_format", std::string()) == "postgres") {
                    replicated = true;
                }
            }
            EXPECT_TRUE(replicated) << "the tbl: value carries source_format";
        }
        node.stop();
    }
    Node restarted(config_for(tmp));
    restarted.start();
    const std::vector<DatasetSummary> datasets = restarted.datasets();
    const DatasetSummary* summary = find_dataset(datasets, csv_table);
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->source_format, "csv");
    restarted.stop();
}

// #1021: a dataset keeps its source's column order — the CSV header, the order
// a JSON Lines or JSON source first writes each key — through get_dataset's
// summary, SELECT *, the replicated document and a restart. Rows are stored as
// JSON objects whose keys read back sorted, so before this every one of these
// came back alphabetical.
TEST(AddDataset, KeepsTheSourceColumnOrder) {
    TempDir tmp;
    std::string csv_table;
    {
        // Replicated, so the capture into the document is exercised too.
        zetta::cluster::ReplicationManager replication{"node-a"};
        Node node(config_for(tmp), &replication);
        node.start();
        const AddDatasetResult csv = add_dataset_file(
            node, write_file(tmp.path(), "accounts.csv",
                             "zeta,alpha,mid\n1,a,\n2,b,3.5\n3,c,4\n"));
        ASSERT_TRUE(csv.ok()) << csv.error;
        csv_table = csv.table->id();
        EXPECT_EQ(csv.table->column_order(),
                  (std::vector<std::string>{"zeta", "alpha", "mid"}));

        const AddDatasetResult jsonl = zetta::data::add_dataset_content(
            node, "{\"b\":1,\"a\":2}\n{\"c\":3,\"b\":4,\"a\":5}\n", Format::Jsonl,
            {.table_name = "events"});
        ASSERT_TRUE(jsonl.ok()) << jsonl.error;
        EXPECT_EQ(jsonl.table->column_order(), (std::vector<std::string>{"b", "a", "c"}))
            << "a later row's new key follows, in the order that row writes it";

        const AddDatasetResult json = zetta::data::add_dataset_content(
            node, "[{\"y\":1,\"x\":2},{\"x\":3,\"w\":4}]", Format::Json,
            {.table_name = "points"});
        ASSERT_TRUE(json.ok()) << json.error;
        EXPECT_EQ(json.table->column_order(), (std::vector<std::string>{"y", "x", "w"}));

        // A caller's order (a connector's SELECT list) wins over the decoder's.
        const AddDatasetResult chosen = zetta::data::add_dataset_content(
            node, "p,q\n1,2\n", Format::Csv,
            {.table_name = "chosen", .columns = {"q", "p"}});
        ASSERT_TRUE(chosen.ok()) << chosen.error;
        EXPECT_EQ(chosen.table->column_order(), (std::vector<std::string>{"q", "p"}));

        // In-memory rows hold no order of their own: nothing is recorded.
        const AddDatasetResult rows = add_dataset_rows(
            node, std::vector<nlohmann::json>{{{"z", 1}, {"a", 2}}}, {});
        ASSERT_TRUE(rows.ok()) << rows.error;
        EXPECT_TRUE(rows.table->column_order().empty());

        const std::vector<DatasetSummary> datasets = node.datasets();
        const DatasetSummary* summary = find_dataset(datasets, csv_table);
        ASSERT_NE(summary, nullptr);
        EXPECT_TRUE(summary->column_order_recorded);
        EXPECT_EQ(summary->columns, (std::vector<std::string>{"zeta", "alpha", "mid"}));
        ASSERT_EQ(summary->typed_columns.size(), 3u);
        EXPECT_EQ(summary->typed_columns[0].type, "integer");
        EXPECT_EQ(summary->typed_columns[1].type, "string");
        EXPECT_EQ(summary->typed_columns[2].type, "number");
        EXPECT_TRUE(summary->typed_columns[2].nullable);
        ASSERT_TRUE(summary->typed_columns[2].null_fraction);
        EXPECT_NEAR(*summary->typed_columns[2].null_fraction, 1.0 / 3.0, 1e-9);
        const DatasetSummary* unordered = find_dataset(datasets, rows.table->id());
        ASSERT_NE(unordered, nullptr);
        EXPECT_FALSE(unordered->column_order_recorded);
        EXPECT_EQ(unordered->columns, (std::vector<std::string>{"a", "z"}));

        const nlohmann::json document = replication.snapshot();
        const auto key = document.find("cols:" + csv_table);
        ASSERT_NE(key, document.end()) << "the order is captured under cols:<table-id>";
        EXPECT_EQ(key->at("columns"), nlohmann::json::array({"zeta", "alpha", "mid"}));
        // A reconcile pass keeps it (the document is the source of truth).
        node.tick();
        EXPECT_EQ(csv.table->column_order(),
                  (std::vector<std::string>{"zeta", "alpha", "mid"}));
        // Dropping the table drops its order with it.
        const std::string events_id = jsonl.table->id();
        ASSERT_TRUE(jsonl.database->drop_table("events"));
        EXPECT_FALSE(replication.snapshot().contains("cols:" + events_id));
        node.stop();
    }
    // table.json carries it across a restart, into a fresh document.
    zetta::cluster::ReplicationManager fresh{"node-a"};
    Node restarted(config_for(tmp), &fresh);
    restarted.start();
    const std::vector<DatasetSummary> datasets = restarted.datasets();
    const DatasetSummary* summary = find_dataset(datasets, csv_table);
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->columns, (std::vector<std::string>{"zeta", "alpha", "mid"}));
    EXPECT_TRUE(fresh.snapshot().contains("cols:" + csv_table))
        << "migrated into the document on start";
    restarted.tick();
    const std::vector<DatasetSummary> after_tick = restarted.datasets();
    const DatasetSummary* reconciled = find_dataset(after_tick, csv_table);
    ASSERT_NE(reconciled, nullptr);
    EXPECT_EQ(reconciled->columns, (std::vector<std::string>{"zeta", "alpha", "mid"}));
    restarted.stop();
}

TEST(AddDataset, AParquetFileKeepsItsSchemaOrder) {
    if (!fs::exists(saas_fixture_path())) GTEST_SKIP() << "parquet fixture missing";
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const AddDatasetResult result = add_dataset_file(node, saas_fixture_path());
    ASSERT_TRUE(result.ok()) << result.error;
    std::ifstream in(saas_fixture_path(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    const auto metadata = zetta::parquet::parse_parquet_metadata(bytes);
    ASSERT_TRUE(metadata.ok()) << metadata.error;
    std::vector<std::string> schema_order;
    for (const auto& leaf : metadata.metadata.leaf_columns()) schema_order.push_back(leaf.path);
    ASSERT_FALSE(schema_order.empty());
    EXPECT_EQ(result.table->column_order(), schema_order);
    node.stop();
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
// In-memory rows (add_dataset_rows) — the connector-query landing path
// =====================================================================

TEST(AddDataset, LoadsInMemoryRowsAsPartitionedTable) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    // The shape a connector query yields: already-decoded JSON documents.
    std::vector<nlohmann::json> rows = {
        {{"account_id", "acct_globex"}, {"mrr", 1200}, {"churned", false}},
        {{"account_id", "acct_initech"}, {"mrr", 3400}, {"churned", true}},
        {{"account_id", "acct_acme"}, {"mrr", 560}, {"churned", false}},
    };

    AddDatasetOptions options;
    options.database = "churn";
    options.table_name = "accounts";
    const AddDatasetResult result = add_dataset_rows(node, rows, options);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.format, Format::Jsonl);  // in-memory JSON documents
    EXPECT_EQ(result.rows, 3u);
    ASSERT_TRUE(result.table);
    EXPECT_EQ(result.table->name(), "accounts");
    EXPECT_EQ(result.database->name(), "churn");

    // Partitioned + placed, exactly like the file path.
    ASSERT_FALSE(result.partitions.empty());
    for (const auto& partition : result.partitions) {
        EXPECT_FALSE(partition->owner_nodes().empty());
    }

    // The rows landed verbatim.
    const std::vector<nlohmann::json> stored = table_rows(*result.table);
    ASSERT_EQ(stored.size(), 3u);
    EXPECT_EQ(stored[0]["account_id"], "acct_globex");
    EXPECT_EQ(stored[1]["mrr"], 3400);
    EXPECT_EQ(stored[1]["churned"], true);

    // Visible in system.datasets with its placement. (Bind the vector to a
    // named local — find_dataset returns a pointer into it.)
    const std::vector<DatasetSummary> datasets = node.datasets();
    const DatasetSummary* summary = find_dataset(datasets, result.table->id());
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->rows, 3u);
    EXPECT_FALSE(summary->partitions.empty());

    node.stop();
}

TEST(AddDataset, InMemoryRowsDefaultTableNameAndSpreadAcrossMembers) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    // No table_name → the "dataset" default; explicit membership spreads the
    // partitions across more than one owner (rendezvous placement).
    std::vector<nlohmann::json> rows;
    for (int i = 0; i < 12; ++i) rows.push_back({{"id", i}});

    AddDatasetOptions options;
    options.members = {"node-a", "node-b", "node-c"};
    const AddDatasetResult result = add_dataset_rows(node, rows, options);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.table->name(), "dataset");
    EXPECT_EQ(result.database->name(), "datasets");  // default database

    std::set<std::string> owners;
    for (const auto& partition : result.partitions) {
        for (const std::string& owner : partition->owner_nodes()) owners.insert(owner);
    }
    EXPECT_GT(owners.size(), 1u) << "partitions should spread across the cluster";

    node.stop();
}

TEST(AddDataset, InMemoryRowsRecordTheAuditEvent) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    std::vector<nlohmann::json> rows = {{{"a", 1}}, {{"a", 2}}};
    const AddDatasetResult result = add_dataset_rows(node, rows, {});
    ASSERT_TRUE(result.ok()) << result.error;

    bool audited = false;
    for (const auto& event : node.audit_events()) {
        if (event &&
            event->action() == zetta::governance::AuditAction::DatasetAdded &&
            event->target() == result.table->id()) {
            audited = true;
        }
    }
    EXPECT_TRUE(audited);

    node.stop();
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


// =====================================================================
// The async add (#624): decode on a worker, materialize in owner slices
// =====================================================================

namespace {

// Pump the add on this (owner) thread until it finishes or `seconds` elapse.
void pump_until_done(zetta::Node& node, zetta::data::AsyncDatasetAdd& add,
                     int seconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!add.done() && std::chrono::steady_clock::now() < deadline) {
        node.tick();
        add.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// A CSV big enough that the materialize spans several bounded pump slices
// (> the per-pump chunk), so the tests exercise the sliced path.
std::string big_csv(std::size_t rows) {
    std::string content = "id,value\n";
    for (std::size_t i = 0; i < rows; ++i) {
        content += std::to_string(i) + "," + std::to_string(i * 2) + "\n";
    }
    return content;
}

}  // namespace

TEST(AsyncDatasetAdd, DecodesOffThreadAndMaterializesInSlices) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    constexpr std::size_t kRows = 9000;  // > one materialize chunk
    const fs::path csv = write_file(tmp.path(), "large.csv", big_csv(kRows));

    const std::shared_ptr<zetta::data::AsyncDatasetAdd> add =
        zetta::data::AsyncDatasetAdd::start(node, csv, {});
    pump_until_done(node, *add, 60);
    ASSERT_TRUE(add->done());

    const AddDatasetResult& result = add->result();
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_TRUE(result.table);
    EXPECT_EQ(result.rows, kRows);
    EXPECT_EQ(add->rows_total(), kRows);
    EXPECT_EQ(add->rows_added(), kRows);
    EXPECT_FALSE(result.partitions.empty());
    EXPECT_EQ(result.table->row_count(), kRows);

    // The completed add is audited — the activity feed's completion entry.
    bool audited = false;
    for (const auto& event : node.audit_events()) {
        if (event &&
            event->action() == zetta::governance::AuditAction::DatasetAdded &&
            event->target() == result.table->id()) {
            audited = true;
        }
    }
    EXPECT_TRUE(audited);

    node.stop();
}

TEST(AsyncDatasetAdd, CancelMidMaterializeDropsThePartialTable) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const fs::path csv = write_file(tmp.path(), "big.csv", big_csv(9000));
    const std::shared_ptr<zetta::data::AsyncDatasetAdd> add =
        zetta::data::AsyncDatasetAdd::start(node, csv, {});

    // Reach the materialize phase (decode finished, table created)…
    using Phase = zetta::data::AsyncDatasetAdd::Phase;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (add->phase() == Phase::Decoding && !add->done() &&
           std::chrono::steady_clock::now() < deadline) {
        node.tick();
        add->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(add->phase(), Phase::Materializing);
    add->pump();  // land at least one slice of rows

    // …then cancel: the next pump drops the partial table.
    add->request_cancel();
    pump_until_done(node, *add, 60);
    ASSERT_TRUE(add->done());
    EXPECT_FALSE(add->result().ok());
    EXPECT_EQ(add->result().error, "cancelled by user");
    EXPECT_FALSE(add->result().table);

    // Nothing durable survives: the table is gone from its database.
    const std::shared_ptr<Database> database = node.find_database("datasets");
    ASSERT_TRUE(database);
    EXPECT_FALSE(database->find_table("big"));

    node.stop();
}

TEST(AsyncDatasetAdd, CancelDuringDecodeTouchesNothing) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const fs::path csv = write_file(tmp.path(), "rows.csv", big_csv(9000));
    const std::shared_ptr<zetta::data::AsyncDatasetAdd> add =
        zetta::data::AsyncDatasetAdd::start(node, csv, {});
    // Cancel immediately — before pumping at all, so the decode (or the first
    // pump) observes it and the node is never touched.
    add->request_cancel();
    pump_until_done(node, *add, 60);
    ASSERT_TRUE(add->done());
    EXPECT_FALSE(add->result().ok());
    EXPECT_EQ(add->result().error, "cancelled by user");
    EXPECT_FALSE(node.find_database("datasets"))
        << "a cancel before materialize must not create the database";

    node.stop();
}

TEST(AddDataset, SyncAddRecordsTheAuditEvent) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    const fs::path csv =
        write_file(tmp.path(), "tiny.csv", "a,b\n1,2\n3,4\n");
    const AddDatasetResult result = add_dataset_file(node, csv);
    ASSERT_TRUE(result.ok()) << result.error;
    bool audited = false;
    for (const auto& event : node.audit_events()) {
        if (event &&
            event->action() == zetta::governance::AuditAction::DatasetAdded &&
            event->target() == result.table->id()) {
            audited = true;
        }
    }
    EXPECT_TRUE(audited);

    node.stop();
}

// #833 section 8 — a durable dataset is RF=3 by default, clamped to the node
// count. It used to be RF=1, which was harmless only while a partitioned table
// also replicated every row through the CRDT; once rows stop being captured the
// factor is the only thing between a dataset and one copy with no backup.
TEST(AddDataset, ReplicationFactorDefaultsToThreeClampedToTheNodeCount) {
    EXPECT_EQ(AddDatasetOptions{}.replication_factor, 3u);

    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    std::vector<nlohmann::json> rows;
    for (int i = 0; i < 24; ++i) rows.push_back(nlohmann::json{{"n", i}});

    // A five-node roster: the default places three owners per partition.
    AddDatasetOptions five;
    five.table_name = "wide";
    five.partition_count = 4;
    five.members = {"node-1", "node-2", "node-3", "node-4", "node-5"};
    const AddDatasetResult placed = add_dataset_rows(node, rows, five);
    ASSERT_TRUE(placed.ok()) << placed.error;
    ASSERT_FALSE(placed.partitions.empty());
    for (const auto& partition : placed.partitions) {
        EXPECT_EQ(partition->owner_nodes().size(), 3u);
    }

    // A two-node roster clamps to two — recording three owners where only two
    // nodes exist would be a placement onto a node that is not there.
    AddDatasetOptions two;
    two.table_name = "narrow";
    two.partition_count = 4;
    two.members = {"node-1", "node-2"};
    const AddDatasetResult clamped = add_dataset_rows(node, rows, two);
    ASSERT_TRUE(clamped.ok()) << clamped.error;
    ASSERT_FALSE(clamped.partitions.empty());
    for (const auto& partition : clamped.partitions) {
        EXPECT_EQ(partition->owner_nodes().size(), 2u);
    }

    // A single-node cluster (no roster given) still places successfully.
    AddDatasetOptions alone;
    alone.table_name = "solo";
    alone.partition_count = 4;
    const AddDatasetResult single = add_dataset_rows(node, rows, alone);
    ASSERT_TRUE(single.ok()) << single.error;
    ASSERT_FALSE(single.partitions.empty());
    for (const auto& partition : single.partitions) {
        EXPECT_EQ(partition->owner_nodes().size(), 1u);
    }

    node.stop();
}

// #1021: partition stats arrive through the replicated catalog, so a field of
// the wrong shape is read as unknown, never thrown on (Node::datasets() would
// otherwise fail every get_dataset and list_datasets).
TEST(AddDataset, ColumnProfileReadsMalformedPartitionStatsWithoutThrowing) {
    zetta::data::ColumnProfile profile;
    EXPECT_NO_THROW(profile.add_partition_stats(
        nlohmann::json{{"column_stats",
                        {{{"name", "a"}, {"type", 7}, {"nulls", "two"}},
                         {{"name", "b"}, {"type", "integer"}, {"nulls", -1}}}}},
        4));
    EXPECT_NO_THROW(profile.add_partition_stats(
        nlohmann::json{{"schema", {{{"name", "c"}, {"type", nullptr}, {"optional", "yes"}}}}},
        2));
    std::vector<zetta::data::DatasetColumn> columns;
    ASSERT_NO_THROW(columns = profile.columns({"a", "b", "c"}));
    ASSERT_EQ(columns.size(), 3u);
    EXPECT_EQ(columns[0].type, "null");
    EXPECT_EQ(columns[1].type, "integer");
    EXPECT_TRUE(columns[2].nullable);
}

// --- DatasetLineDecoder and the object load (#1302) ---------------------------

// Feed `text` to a decoder a '\n'-separated line at a time, as a streaming load
// does. The error, or empty, and the rows decoded.
std::string decode_by_line(Format format, const std::string& text,
                           std::vector<nlohmann::json>& rows) {
    zetta::data::DatasetLineDecoder decoder(format);
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (!decoder.line(std::string_view(text).substr(start, end - start), rows)) {
            return decoder.error();
        }
        start = end + 1;
    }
    return decoder.finish() ? std::string() : decoder.error();
}

// The decoder gives the rows and the refusals, word for word, that a whole-file
// load gives for the same text, because every load path decodes through it.
TEST(DatasetLineDecoder, MatchesTheWholeTextDecodersRowsAndRefusals) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    const std::vector<std::pair<std::string, std::string>> refused_csv = {
        {"a,b\n1,2\n3\n", "line 3: 1 fields, but the header has 2 columns"},
        {"\na,b\n1,2,3\n", "line 3: 3 fields, but the header has 2 columns"},
        {"a,,c\n1,2,3\n", "line 1: header column 2 has no name"},
        {"id,id\n1,2\n", "line 1: the header names column 'id' twice"},
        {"a,b\n1,2\n\n\n\n\n\n\n\"x,1\n", "line 9: a quoted field is not closed (a row must stay on one line)"},
        {"\r\n\r\n", "the file has no header row"},
    };
    int index = 0;
    for (const auto& [text, expected] : refused_csv) {
        std::vector<nlohmann::json> rows;
        EXPECT_EQ(decode_by_line(Format::Csv, text, rows), expected) << text;
        const AddDatasetResult whole = add_dataset_file(
            node, write_file(tmp.path(), "refused" + std::to_string(index++) + ".csv", text));
        EXPECT_EQ(whole.error, expected) << text;
        EXPECT_EQ(zetta::data::csv_problem(text), expected) << text;
    }
    std::vector<nlohmann::json> jsonl_rows;
    EXPECT_EQ(decode_by_line(Format::Jsonl, "{\"ok\":1}\n\n{not json}\n", jsonl_rows),
              "invalid JSON on line 3");
    EXPECT_EQ(add_dataset_file(node, write_file(tmp.path(), "bad.jsonl",
                                                "{\"ok\":1}\n\n{not json}\n"))
                  .error,
              "invalid JSON on line 3");

    // The same rows, cell for cell, as a whole-file load stores.
    const std::string csv = "id,zip,score,flag,note\r\n1,01234,2.5,true,\"a, b\"\r\n\r\n2,9,,FALSE,x";
    std::vector<nlohmann::json> rows;
    ASSERT_EQ(decode_by_line(Format::Csv, csv, rows), "");
    const AddDatasetResult whole = add_dataset_file(node, write_file(tmp.path(), "good.csv", csv));
    ASSERT_TRUE(whole.ok()) << whole.error;
    EXPECT_EQ(rows, table_rows(*whole.table));
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]["zip"], "01234");
    EXPECT_EQ(rows[0]["note"], "a, b");
    EXPECT_TRUE(rows[1]["score"].is_null());
    node.stop();
}

// A standalone node with an in-memory object store (an empty directory) and a
// bucket, driven from this thread as the owner loop.
struct ObjectLoad : ::testing::Test {
    TempDir tmp;
    Node node{config_for(tmp)};
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::data::ObjectStore objects{replication, replication.node_id(), {}};

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(objects.open());
        ASSERT_TRUE(objects.create_bucket(zetta::data::ObjectBucket{"loads"}).ok());
        node.tick();  // adopt this thread as the owner before any worker runs
    }
    void TearDown() override { node.stop(); }

    void put(const std::string& key, std::string_view bytes) {
        const auto written = objects.put_range("loads", key, 0, bytes, {});
        ASSERT_TRUE(written.ok()) << written.message;
    }

    // Load `key` as `table`, ticking until the run settles.
    std::shared_ptr<zetta::data::AsyncObjectDatasetLoad> load(
        const std::string& key, const std::string& table, Format format = Format::Csv,
        std::uint64_t max_bytes = zetta::data::kMaxDatasetObjectBytes) {
        zetta::data::ObjectDatasetSource source;
        source.bucket = "loads";
        source.key = key;
        source.format = format;
        source.max_bytes = max_bytes;
        AddDatasetOptions options;
        options.table_name = table;
        auto handle =
            zetta::data::AsyncObjectDatasetLoad::start(node, objects, source, options);
        EXPECT_TRUE(handle);
        if (!handle) return handle;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (!handle->done() && std::chrono::steady_clock::now() < deadline) {
            node.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(handle->done());
        return handle;
    }

    std::shared_ptr<zetta::pipeline::Run> run_of(const zetta::data::AsyncObjectDatasetLoad& handle) {
        return node.find_run(handle.run_id());
    }

    bool table_exists(const std::string& table) {
        const auto database = node.find_database("datasets");
        return database && database->find_table(table);
    }
};

// A CSV larger than two slices, whose lines straddle every slice boundary,
// loads every row exactly once, the first and the last intact.
TEST_F(ObjectLoad, LinesThatStraddleSliceBoundariesLoadOnce) {
    std::string csv = "id,label,amount\n";
    std::size_t rows = 0;
    while (csv.size() < std::size_t{9} * 1024 * 1024) {
        csv += std::to_string(rows) + ",row-" + std::to_string(rows) + "-padding," +
               std::to_string(rows * 3) + ".5\n";
        ++rows;
    }
    ASSERT_GT(csv.size(), 2u * zetta::data::ObjectStore::kSliceBytes);
    put("big.csv", csv);

    const auto handle = load("big.csv", "big");
    ASSERT_TRUE(handle);
    ASSERT_TRUE(handle->result().ok()) << handle->result().error;
    const auto run = run_of(*handle);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);
    ASSERT_EQ(run->outputs().size(), 1u);
    EXPECT_EQ(run->outputs()[0].kind, "dataset");
    EXPECT_EQ(run->outputs()[0].name, "big");
    EXPECT_EQ(run->outputs()[0].ref, "datasets.big");
    ASSERT_FALSE(run->logs().empty());
    EXPECT_EQ(run->logs().front().message,
              "loading loads/big.csv: " + std::to_string(csv.size()) + " bytes as csv");
    EXPECT_EQ(run->logs().back().message,
              "loaded " + std::to_string(rows) + " rows into datasets.big in " +
                  std::to_string(handle->result().partitions.size()) + " partitions");

    const auto stored = table_rows(*handle->result().table);
    ASSERT_EQ(stored.size(), rows);
    std::set<long long> ids;
    for (const auto& row : stored) {
        const long long id = row.at("id").get<long long>();
        ids.insert(id);
        EXPECT_EQ(row.at("label"), "row-" + std::to_string(id) + "-padding");
    }
    EXPECT_EQ(ids.size(), rows);
    EXPECT_EQ(*ids.begin(), 0);
    EXPECT_EQ(*ids.rbegin(), static_cast<long long>(rows - 1));
    EXPECT_EQ(handle->result().source_format, "csv");
    // Partition-only (#833): the rows live in the partition bodies, and the
    // manifest says how many there are (what segmentation checks for).
    EXPECT_TRUE(handle->result().table->partition_only());
    EXPECT_TRUE(handle->result().table->partition_manifest());

    bool audited = false;
    for (const auto& event : node.audit_events()) {
        audited = audited || (event && event->action() == zetta::governance::AuditAction::DatasetAdded &&
                              event->target() == handle->result().table->id());
    }
    EXPECT_TRUE(audited);
}

// CRLF endings, blank lines and a last line with no newline, in both formats.
TEST_F(ObjectLoad, CrlfBlankLinesAndAnUnterminatedLastLine) {
    put("crlf.csv", "id,name\r\n\r\n1,one\r\n\r\n2,two\r\n3,three");
    const auto csv = load("crlf.csv", "crlf");
    ASSERT_TRUE(csv && csv->result().ok()) << (csv ? csv->result().error : "");
    const auto csv_rows = table_rows(*csv->result().table);
    ASSERT_EQ(csv_rows.size(), 3u);
    EXPECT_EQ(csv_rows[0], (nlohmann::json{{"id", 1}, {"name", "one"}}));
    EXPECT_EQ(csv_rows[2], (nlohmann::json{{"id", 3}, {"name", "three"}}));

    put("lines.jsonl", "{\"a\":1}\r\n  \r\n\r\n{\"a\":2}\r\n{\"a\":3}");
    const auto jsonl = load("lines.jsonl", "lines", Format::Jsonl);
    ASSERT_TRUE(jsonl && jsonl->result().ok()) << (jsonl ? jsonl->result().error : "");
    const auto jsonl_rows = table_rows(*jsonl->result().table);
    ASSERT_EQ(jsonl_rows.size(), 3u);
    EXPECT_EQ(jsonl_rows[2], (nlohmann::json{{"a", 3}}));
    EXPECT_EQ(jsonl->result().source_format, "jsonl");
}

// A line past kMaxDatasetLineBytes is refused naming it, and leaves no table.
TEST_F(ObjectLoad, ALineLongerThanTheLimitFailsAndLeavesNoTable) {
    std::string csv = "a\n1\n";
    csv += std::string(zetta::data::kMaxDatasetLineBytes + 1, 'x');
    csv += "\n2\n";
    put("long.csv", csv);
    const auto handle = load("long.csv", "long_line");
    ASSERT_TRUE(handle);
    EXPECT_EQ(handle->result().error, "line 3 is longer than 16 MiB");
    const auto run = run_of(*handle);
    EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Failed);
    EXPECT_EQ(run->logs().back().level, "error");
    EXPECT_EQ(run->logs().back().message, "line 3 is longer than 16 MiB");
    EXPECT_FALSE(table_exists("long_line"));
}

// A ragged row on the last line fails after earlier batches created and filled
// the table, and that table is dropped.
TEST_F(ObjectLoad, ARaggedLastRowDropsThePartiallyFilledTable) {
    std::string csv = "id,value\n";
    for (int row = 0; row < 25'000; ++row) {
        csv += std::to_string(row) + "," + std::to_string(row * 2) + "\n";
    }
    csv += "25000\n";
    put("ragged.csv", csv);
    const auto handle = load("ragged.csv", "ragged");
    ASSERT_TRUE(handle);
    EXPECT_EQ(handle->result().error, "line 25002: 1 fields, but the header has 2 columns");
    EXPECT_EQ(run_of(*handle)->status(), zetta::pipeline::RunStatus::Failed);
    EXPECT_FALSE(table_exists("ragged"));
}

// An object over the limit fails before any row is read; the limit comes in
// through the source (a 1 GiB fixture would be too heavy).
TEST_F(ObjectLoad, AnObjectOverTheLimitFailsBeforeAnyRowIsRead) {
    put("over.csv", "a,b\n1,2\n3,4\n");
    const auto handle = load("over.csv", "over", Format::Csv, /*max_bytes=*/8);
    ASSERT_TRUE(handle);
    EXPECT_EQ(handle->result().error,
              "the object is 12 bytes; a dataset loads from at most 8 bytes");
    EXPECT_EQ(handle->result().rows, 0u);
    EXPECT_FALSE(table_exists("over"));
    EXPECT_FALSE(node.find_database("datasets"));
}

// The other refusals, each in its words, none leaving a table.
TEST_F(ObjectLoad, RefusalsNameTheReasonAndLeaveNoTable) {
    const auto missing = load("absent.csv", "absent");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->result().error, "no object absent.csv in bucket loads");

    put("header.csv", "a,b\n");
    EXPECT_EQ(load("header.csv", "header")->result().error, "the object decoded to no rows");

    put("bad.jsonl", "{\"a\":1}\n{nope}\n");
    EXPECT_EQ(load("bad.jsonl", "bad", Format::Jsonl)->result().error, "invalid JSON on line 2");

    put("unclosed.csv", "a,b\n1,\"2\n");
    EXPECT_EQ(load("unclosed.csv", "unclosed")->result().error,
              "line 2: a quoted field is not closed (a row must stay on one line)");

    for (const char* table : {"absent", "header", "bad", "unclosed"}) {
        EXPECT_FALSE(table_exists(table)) << table;
    }
}

// A name taken while the object was read is refused when the first batch would
// create the table, unless the caller asked for a rename.
TEST_F(ObjectLoad, ATakenNameFailsUnlessRenameWasAsked) {
    put("one.csv", "a\n1\n");
    ASSERT_TRUE(load("one.csv", "taken")->result().ok());
    const auto refused = load("one.csv", "taken");
    EXPECT_EQ(refused->result().error, zetta::data::dataset_name_taken("taken", "datasets"));

    zetta::data::ObjectDatasetSource source{"loads", "one.csv", Format::Csv};
    source.rename_if_exists = true;
    AddDatasetOptions options;
    options.table_name = "taken";
    auto renamed = zetta::data::AsyncObjectDatasetLoad::start(node, objects, source, options);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!renamed->done() && std::chrono::steady_clock::now() < deadline) node.tick();
    ASSERT_TRUE(renamed->result().ok()) << renamed->result().error;
    EXPECT_EQ(renamed->result().table->name(), "taken_2");
    EXPECT_EQ(run_of(*renamed)->logs().back().message,
              "loaded 1 rows into datasets.taken_2 in " +
                  std::to_string(renamed->result().partitions.size()) +
                  " partitions (asked for taken; that name was taken)");
}

}  // namespace
