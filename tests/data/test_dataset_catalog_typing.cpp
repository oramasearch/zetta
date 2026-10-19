#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/node.hpp"

// **Typing the dataset contract from the partition catalog (#1203 item 3).**
//
// The contract used to be typed by `SELECT *` over the whole table, keeping the
// first 256 rows. Each partition body records its column types in the catalog
// (#837), so resolve_dataset now takes the types from there and reads values only
// for what the catalog cannot decide: text (is it a category?), and a label or
// entity candidate whose 0/1-ness or whole-ness the catalog does not record.
//
// The rule these tests hold it to: **the catalog changes what is read, never what
// is decided.** The same rows typed from the catalog and typed from a sample give
// the same contract, down to the categories' fitted levels.
namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::data::CatalogColumnType;
using zetta::data::CatalogColumnTypes;
using zetta::data::DatasetContract;
using zetta::data::DatasetPartition;
using zetta::data::DatasetPartitionValue;
using zetta::data::ResolvedDataset;
using Json = nlohmann::json;

struct AttachedNode {
    explicit AttachedNode(std::uint16_t port)
        : node(node_config(temp_dir)), cluster("catalog-typing", cluster_config(port)) {
        node.start();
        cluster.start();
        node.attach_cluster(cluster, /*worker_count=*/2);
    }
    ~AttachedNode() { node.stop(); }
    static NodeConfig node_config(const zetta::test::TempDir& dir) {
        NodeConfig config;
        config.data_dir = dir.path().string();
        return config;
    }
    static ClusterConfig cluster_config(std::uint16_t port) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        return config;
    }
    zetta::test::TempDir temp_dir;
    Node node;
    Cluster cluster;
};

// A node with no cluster.
struct LoneNode {
    LoneNode() : node(AttachedNode::node_config(temp_dir)) { node.start(); }
    ~LoneNode() { node.stop(); }
    zetta::test::TempDir temp_dir;
    Node node;
};

// Every kind of column inference distinguishes, in one table: an integer entity, a
// 0/1 integer label, an integer and a real feature, a boolean, a column that is
// whole in most rows and real in a few, two categories (one with nulls), an
// identifier, text dates, and a column with no value at all.
std::vector<Json> mixed_rows(std::size_t count) {
    const std::vector<std::string> plans{"basic", "pro", "enterprise"};
    const std::vector<std::string> regions{"north", "south", "east", "west"};
    std::vector<Json> rows;
    for (std::size_t index = 0; index < count; ++index) {
        rows.push_back(Json{
            {"account_id", static_cast<std::int64_t>(index + 1)},
            {"churned", index % 4 == 0 ? 1 : 0},
            {"seats", static_cast<std::int64_t>(index % 40)},
            {"spend", static_cast<double>(index % 17) + 0.25},
            {"active", index % 2 == 0},
            {"usage", index % 9 == 0 ? Json(static_cast<double>(index) + 0.5)
                                     : Json(static_cast<std::int64_t>(index))},
            {"plan", plans[index % 3]},
            {"region", index % 5 == 0 ? Json(nullptr) : Json(regions[index % 4])},
            {"email", "user" + std::to_string(index) + "@example.com"},
            {"signed_up", "2026-0" + std::to_string(1 + index % 9) + "-01"},
            {"notes", nullptr}});
    }
    return rows;
}

zetta::data::AddDatasetResult land(AttachedNode& host, std::vector<Json> rows,
                                   const std::string& table, bool partitioned) {
    zetta::data::AddDatasetOptions options;
    options.database = "typing";
    options.table_name = table;
    options.partitioned = partitioned;
    if (partitioned) options.partition_count = 4;
    options.members = {host.node.node_id()};
    return zetta::data::add_dataset_rows(host.node, std::move(rows), options);
}

ResolvedDataset resolve(AttachedNode& host, const std::string& table,
                        const DatasetContract& overrides = {},
                        const zetta::data::DatasetSampler& sampler = {}) {
    return zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("typing." + table), overrides, sampler,
        zetta::data::kDefaultSampleRows, &host.cluster);
}

// The contract without the parts that differ by construction between two tables
// holding the same rows (nothing today; kept as one place to say so).
void expect_same_contract(const DatasetContract& catalog_typed,
                          const DatasetContract& sample_typed) {
    EXPECT_EQ(catalog_typed.entity_column, sample_typed.entity_column);
    EXPECT_EQ(catalog_typed.label_column, sample_typed.label_column);
    EXPECT_EQ(catalog_typed.feature_columns, sample_typed.feature_columns);
    EXPECT_EQ(catalog_typed.categorical_features, sample_typed.categorical_features);
    EXPECT_EQ(catalog_typed, sample_typed);
}

std::shared_ptr<DatasetPartition> partition(const std::string& id, std::uint64_t start,
                                            std::uint64_t end, Json schema) {
    DatasetPartitionValue value;
    value.table = "tbl-1";
    value.row_range = {start, end};
    if (!schema.is_null()) value.stats = Json{{"schema", std::move(schema)}};
    return std::make_shared<DatasetPartition>(id, std::move(value), "/nonexistent");
}

Json column(const std::string& name, const std::string& type) {
    return Json{{"name", name}, {"type", type}, {"optional", true}};
}

}  // namespace

// --- the catalog's types -------------------------------------------------------

TEST(CatalogTyping, PartitionTypesMergeAcrossTheTable) {
    const std::vector<std::shared_ptr<DatasetPartition>> partitions = {
        partition("p1", 0, 10,
                  Json::array({column("id", "int64"), column("flag", "boolean"),
                               column("amount", "int64"), column("plan", "byte_array"),
                               column("mixed", "int64"), column("later", "null"),
                               column("_zid", "byte_array")})),
        partition("p2", 10, 25,
                  Json::array({column("id", "int64"), column("flag", "int64"),
                               column("amount", "double"), column("plan", "byte_array"),
                               column("mixed", "string"), column("later", "number")})),
        // A partition of another table is not this table's business.
        [] {
            DatasetPartitionValue value;
            value.table = "tbl-other";
            value.row_range = {0, 99};
            return std::make_shared<DatasetPartition>("p9", std::move(value), "/nonexistent");
        }(),
    };
    const CatalogColumnTypes catalog = zetta::data::catalog_column_types(partitions, "tbl-1", 25);
    ASSERT_TRUE(catalog.available()) << catalog.unavailable;
    EXPECT_EQ(catalog.partitions, 2u);
    EXPECT_EQ(catalog.type_of("id"), CatalogColumnType::Integer);
    EXPECT_EQ(catalog.type_of("flag"), CatalogColumnType::Integer);   // boolean + integer
    EXPECT_EQ(catalog.type_of("amount"), CatalogColumnType::Number);  // integer + real
    EXPECT_EQ(catalog.type_of("plan"), CatalogColumnType::Undecided);
    EXPECT_EQ(catalog.type_of("mixed"), CatalogColumnType::Undecided);
    EXPECT_EQ(catalog.type_of("later"), CatalogColumnType::Number);  // null + number
    EXPECT_EQ(catalog.type_of("not_recorded"), CatalogColumnType::Undecided);
}

TEST(CatalogTyping, ACatalogThatDoesNotCoverEveryRowCannotTypeTheTable) {
    const Json schema = Json::array({column("id", "int64")});
    const std::vector<std::shared_ptr<DatasetPartition>> partitions = {
        partition("p1", 0, 10, schema), partition("p2", 10, 20, schema)};

    // Rows inserted after the ingest: the catalog no longer describes them.
    const CatalogColumnTypes short_catalog =
        zetta::data::catalog_column_types(partitions, "tbl-1", 30);
    EXPECT_FALSE(short_catalog.available());
    EXPECT_EQ(short_catalog.unavailable, "the partition catalog covers 20 of the table's 30 rows");
    EXPECT_TRUE(short_catalog.columns.empty());

    // A partition written before the catalog recorded schemas.
    const std::vector<std::shared_ptr<DatasetPartition>> older = {
        partition("p1", 0, 10, schema), partition("p-old", 10, 20, Json())};
    const CatalogColumnTypes old_catalog = zetta::data::catalog_column_types(older, "tbl-1", 20);
    EXPECT_FALSE(old_catalog.available());
    EXPECT_EQ(old_catalog.unavailable, "partition p-old records no column types in the catalog");

    const CatalogColumnTypes none = zetta::data::catalog_column_types({}, "tbl-1", 20);
    EXPECT_EQ(none.unavailable, "the table has no partitions in the catalog");
}

TEST(CatalogTyping, OnlyWhatTheCatalogCannotDecideNeedsValues) {
    CatalogColumnTypes catalog;
    catalog.partitions = 1;
    catalog.columns = {{"account_id", CatalogColumnType::Integer},
                       {"customer_id", CatalogColumnType::Number},
                       {"churned", CatalogColumnType::Integer},
                       {"label", CatalogColumnType::Boolean},
                       {"spend", CatalogColumnType::Number},
                       {"seats", CatalogColumnType::Integer},
                       {"plan", CatalogColumnType::Undecided},
                       {"empty", CatalogColumnType::Null}};
    const std::vector<std::string> columns = {"account_id", "customer_id", "churned", "label",
                                              "spend",      "seats",       "plan",    "empty"};

    // Nothing named: the 0/1 label candidate, the real-valued entity candidate,
    // and the text. A boolean label candidate is binary by its type.
    EXPECT_EQ(zetta::data::columns_needing_values(columns, catalog, {}),
              (std::vector<std::string>{"customer_id", "churned", "plan"}));

    // Label and an integer entity named: only the text.
    DatasetContract named;
    named.entity_column = "account_id";
    named.label_column = "churned";
    EXPECT_EQ(zetta::data::columns_needing_values(columns, catalog, named),
              (std::vector<std::string>{"plan"}));

    // A real-valued entity, named: its values decide whether it is a key.
    named.entity_column = "SPEND";
    EXPECT_EQ(zetta::data::columns_needing_values(columns, catalog, named),
              (std::vector<std::string>{"spend", "plan"}));
}

// --- resolve_dataset over a landed table ---------------------------------------

// The catalog-typed contract is the sample-typed one: the same rows landed
// partitioned (typed from the catalog) and unpartitioned (typed from a sample, as
// before) resolve to equal contracts, categories and fitted levels included. A
// partition-only table, read through the scatter-gather select, types the same.
TEST(CatalogTyping, TypesMatchWhatTheSamplerProduced) {
    AttachedNode host(24761);
    ASSERT_TRUE(land(host, mixed_rows(600), "partitioned", true).ok());
    ASSERT_TRUE(land(host, mixed_rows(600), "unpartitioned", false).ok());
    LoneNode lone_host;
    Node& lone = lone_host.node;
    zetta::data::AddDatasetOptions lone_options;
    lone_options.database = "typing";
    lone_options.table_name = "partition_only";
    lone_options.partition_count = 4;
    lone_options.partition_only = true;
    ASSERT_TRUE(zetta::data::add_dataset_rows(lone, mixed_rows(600), lone_options).ok());

    DatasetContract time_cut;
    time_cut.time_column = "signed_up";
    time_cut.as_of = 1777593600.0;  // 2026-05-01: drops the rows signed up later
    for (const DatasetContract& overrides : {DatasetContract{}, time_cut}) {
        const ResolvedDataset catalog_typed = resolve(host, "partitioned", overrides);
        const ResolvedDataset sample_typed = resolve(host, "unpartitioned", overrides);
        ASSERT_TRUE(catalog_typed.ok()) << catalog_typed.error;
        ASSERT_TRUE(sample_typed.ok()) << sample_typed.error;

        EXPECT_TRUE(catalog_typed.reads.typed_from_catalog);
        EXPECT_EQ(catalog_typed.reads.catalog_partitions, 4u);
        EXPECT_FALSE(sample_typed.reads.typed_from_catalog);
        expect_same_contract(catalog_typed.contract, sample_typed.contract);

        // A node with no cluster answers a partition-only table from the
        // partitions it holds (the single-node scatter-gather reference).
        const ResolvedDataset partition_only = zetta::data::resolve_dataset(
            lone, zetta::data::parse_dataset_path("typing.partition_only"), overrides);
        ASSERT_TRUE(partition_only.ok()) << partition_only.error;
        EXPECT_TRUE(partition_only.reads.typed_from_catalog);
        expect_same_contract(partition_only.contract, sample_typed.contract);

        EXPECT_EQ(catalog_typed.contract.entity_column, "account_id");
        EXPECT_EQ(catalog_typed.contract.label_column, "churned");
        EXPECT_EQ(catalog_typed.contract.feature_columns,
                  (std::vector<std::string>{"active", "seats", "spend", "usage"}));
        ASSERT_EQ(catalog_typed.contract.categorical_features.size(), 2u);
        EXPECT_EQ(catalog_typed.contract.categorical_features[0].column, "plan");
        EXPECT_EQ(catalog_typed.contract.categorical_features[1].column, "region");

        // Every note but the first (which says how the columns were typed) is the
        // same decision; the label's and entity's reasons name their source.
        ASSERT_EQ(catalog_typed.notes.size(), sample_typed.notes.size());
        EXPECT_TRUE(catalog_typed.notes.front().starts_with(
            "column types: from the partition catalog (4 partitions); values of "))
            << catalog_typed.notes.front();
        for (std::size_t index = 1; index < catalog_typed.notes.size(); ++index) {
            const std::string& note = catalog_typed.notes[index];
            if (note.starts_with("entity column:")) continue;  // basis differs
            EXPECT_EQ(note, sample_typed.notes[index]);
        }
    }
}

// What is read: text columns and the 0/1 label candidate from the first 256 rows,
// and, over every row, only the categories (plus the time column when there is
// one). Numeric columns are never read to type the contract.
TEST(CatalogTyping, OnlyTheColumnsThatNeedValuesAreRead) {
    AttachedNode host(24762);
    ASSERT_TRUE(land(host, mixed_rows(600), "accounts", true).ok());

    const ResolvedDataset inferred = resolve(host, "accounts");
    ASSERT_TRUE(inferred.ok()) << inferred.error;
    EXPECT_TRUE(inferred.reads.typed_from_catalog);
    // `notes` has no value anywhere, so it is recorded as an all-null Parquet
    // byte_array: undecided, and read like text. It is reported exactly as the
    // sample reported it.
    EXPECT_EQ(inferred.reads.sampled_columns,
              (std::vector<std::string>{"churned", "email", "notes", "plan", "region",
                                        "signed_up"}));
    EXPECT_EQ(inferred.reads.sampled_rows, zetta::data::kDefaultSampleRows);
    EXPECT_EQ(inferred.reads.fitted_columns, (std::vector<std::string>{"plan", "region"}));
    EXPECT_EQ(inferred.reads.fitted_rows, 600u);
    EXPECT_NE(std::ranges::find(inferred.notes, "not used: 'notes' (no values in the sampled rows)"),
              inferred.notes.end());

    DatasetContract named;
    named.entity_column = "account_id";
    named.label_column = "churned";
    named.time_column = "signed_up";
    named.as_of = 1777593600.0;
    const ResolvedDataset with_cutoff = resolve(host, "accounts", named);
    ASSERT_TRUE(with_cutoff.ok()) << with_cutoff.error;
    EXPECT_EQ(with_cutoff.reads.sampled_columns,
              (std::vector<std::string>{"email", "notes", "plan", "region", "signed_up"}));
    EXPECT_EQ(with_cutoff.reads.fitted_columns,
              (std::vector<std::string>{"plan", "region", "signed_up"}));
}

// A table of numbers and a boolean label is typed without reading a row: the
// injected sampler is never called.
TEST(CatalogTyping, ANumericTableReadsNoRowsToType) {
    AttachedNode host(24763);
    std::vector<Json> rows;
    for (std::size_t index = 0; index < 300; ++index) {
        rows.push_back(Json{{"account_id", static_cast<std::int64_t>(index + 1)},
                            {"churned", index % 3 == 0},
                            {"seats", static_cast<std::int64_t>(index % 12)},
                            {"spend", static_cast<double>(index % 17) + 0.5}});
    }
    ASSERT_TRUE(land(host, std::move(rows), "numeric", true).ok());

    std::size_t calls = 0;
    const zetta::data::DatasetSampler counting =
        [&calls](const std::string&, const std::string&, std::size_t, std::string&) {
            ++calls;
            return std::vector<Json>{};
        };
    const ResolvedDataset resolved = resolve(host, "numeric", {}, counting);
    ASSERT_TRUE(resolved.ok()) << resolved.error;
    EXPECT_EQ(calls, 0u);
    EXPECT_TRUE(resolved.reads.sampled_columns.empty());
    EXPECT_TRUE(resolved.reads.fitted_columns.empty());
    EXPECT_EQ(resolved.contract.label_column, "churned");
    EXPECT_EQ(resolved.contract.entity_column, "account_id");
    EXPECT_EQ(resolved.contract.feature_columns, (std::vector<std::string>{"seats", "spend"}));
    EXPECT_EQ(resolved.notes.front(),
              "column types: from the partition catalog (4 partitions); no rows read to type "
              "them");
    EXPECT_NE(std::ranges::find(resolved.notes,
                                "label column: 'churned' (binary by its type in the partition "
                                "catalog, boolean)"),
              resolved.notes.end());
}

// A table the catalog cannot type is typed from sampled rows, every column, as
// before, and the first note says so and why.
TEST(CatalogTyping, ATableWithNoCatalogTypesFallsBackAndSaysSo) {
    AttachedNode host(24764);
    ASSERT_TRUE(land(host, mixed_rows(300), "plain", false).ok());

    const ResolvedDataset resolved = resolve(host, "plain");
    ASSERT_TRUE(resolved.ok()) << resolved.error;
    EXPECT_FALSE(resolved.reads.typed_from_catalog);
    EXPECT_EQ(resolved.reads.catalog_unavailable, "the table has no partitions in the catalog");
    EXPECT_EQ(resolved.reads.sampled_columns.size(), 11u);  // every column
    EXPECT_EQ(resolved.reads.sampled_rows, zetta::data::kDefaultSampleRows);
    EXPECT_EQ(resolved.notes.front(),
              "column types: from the first 256 rows, every column (the table has no "
              "partitions in the catalog)");
    EXPECT_EQ(resolved.contract.label_column, "churned");
    EXPECT_EQ(resolved.contract.categorical_features.size(), 2u);
}

// The projected read answers what `SELECT *` answers for the same columns, and a
// name the SQL subset cannot write as a bare identifier falls back to `SELECT *`.
TEST(CatalogTyping, AProjectedReadMatchesTheWholeRowRead) {
    AttachedNode host(24765);
    std::vector<Json> rows;
    for (std::size_t index = 0; index < 50; ++index) {
        rows.push_back(Json{{"id", static_cast<std::int64_t>(index)},
                            {"plan tier", index % 2 == 0 ? "gold" : "silver"},
                            {"limit", static_cast<std::int64_t>(index * 2)},
                            {"region", index % 3 == 0 ? Json(nullptr) : Json("east")}});
    }
    ASSERT_TRUE(land(host, std::move(rows), "odd_names", true).ok());

    std::string error;
    const std::vector<Json> whole = zetta::data::default_dataset_sampler(
        host.node, &host.cluster)("typing", "odd_names", 0, error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(whole.size(), 50u);

    for (const std::vector<std::string>& columns :
         {std::vector<std::string>{"region", "id"}, std::vector<std::string>{"plan tier"},
          std::vector<std::string>{"limit", "region"}}) {
        const std::vector<Json> projected = zetta::data::read_dataset_columns(
            host.node, &host.cluster, "typing", "odd_names", columns, 20, error);
        ASSERT_TRUE(error.empty()) << error;
        ASSERT_EQ(projected.size(), 20u);
        for (std::size_t index = 0; index < projected.size(); ++index) {
            for (const std::string& name : columns) {
                const Json expected = whole[index].value(name, Json(nullptr));
                const Json actual = projected[index].value(name, Json(nullptr));
                EXPECT_EQ(actual, expected) << name << " row " << index;
            }
            for (const auto& [key, value] : projected[index].items()) {
                EXPECT_NE(std::ranges::find(columns, key), columns.end())
                    << "unasked column " << key;
            }
        }
    }
}

// The levels are fitted on the training rows only: a level that first appears
// after the `as_of` cutoff is not one of the model's levels (it encodes as all
// zeros at predict time), and the note counts the training rows, not the table.
TEST(CatalogTyping, LevelsAfterTheCutoffAreNotFitted) {
    AttachedNode host(24766);
    std::vector<Json> rows;
    for (std::size_t index = 0; index < 400; ++index) {
        const bool late = index >= 300;  // signed up after the cutoff
        rows.push_back(Json{
            {"account_id", static_cast<std::int64_t>(index + 1)},
            {"churned", index % 4 == 0 ? 1 : 0},
            {"seats", static_cast<std::int64_t>(index % 40)},
            {"plan", late && index % 2 == 0 ? "enterprise" : (index % 3 == 0 ? "pro" : "basic")},
            {"signed_up", late ? "2026-06-01" : "2026-04-01"}});
    }
    ASSERT_TRUE(land(host, std::move(rows), "late_levels", true).ok());

    DatasetContract named;
    named.entity_column = "account_id";
    named.label_column = "churned";
    named.time_column = "signed_up";
    named.as_of = 1777593600.0;  // 2026-05-01
    const ResolvedDataset resolved = resolve(host, "late_levels", named);
    ASSERT_TRUE(resolved.ok()) << resolved.error;
    EXPECT_EQ(resolved.reads.fitted_rows, 400u);  // read every row, fit on 300
    ASSERT_EQ(resolved.contract.categorical_features.size(), 1u);
    EXPECT_EQ(resolved.contract.categorical_features[0].column, "plan");
    EXPECT_EQ(resolved.contract.categorical_features[0].levels,
              (std::vector<std::string>{"basic", "pro"}));
    EXPECT_NE(std::ranges::find_if(resolved.notes,
                                   [](const std::string& note) {
                                       return note.starts_with("categorical column: 'plan'") &&
                                              note.find("training rows (300)") !=
                                                  std::string::npos;
                                   }),
              resolved.notes.end());

    // Without the cutoff every row is a training row, and the late level is fitted.
    const ResolvedDataset uncut = resolve(host, "late_levels");
    ASSERT_TRUE(uncut.ok()) << uncut.error;
    ASSERT_EQ(uncut.contract.categorical_features.size(), 1u);
    EXPECT_EQ(uncut.contract.categorical_features[0].levels,
              (std::vector<std::string>{"basic", "enterprise", "pro"}));
}
