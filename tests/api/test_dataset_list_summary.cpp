// SPDX-License-Identifier: MIT

// list_datasets' summary form (#1195): the fields a dataset list shows, from
// counters the engine keeps, reading no row and no file. Each test compares the
// summary with the full form, field by field, so the two cannot drift; the
// read-counting engine and the deleted chunk files prove what the summary does
// not touch.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/util/uuid.hpp"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

// Wraps a table's engine and counts every row it hands out: the test double
// point that shows a path reads no row data.
class CountingEngine final : public zetta::storage::StorageEngine {
public:
    explicit CountingEngine(std::unique_ptr<zetta::storage::StorageEngine> inner)
        : inner_(std::move(inner)) {}

    const char* name() const override { return inner_->name(); }
    void add(std::string id, json doc) override { inner_->add(std::move(id), std::move(doc)); }
    void update(const std::string& id, json doc) override { inner_->update(id, std::move(doc)); }
    void remove(const std::string& id) override { inner_->remove(id); }
    bool contains(const std::string& id) const override { return inner_->contains(id); }
    std::size_t size() const override { return inner_->size(); }
    zetta::storage::ParquetLazyPtr at(std::size_t i) const override {
        ++reads_;
        return inner_->at(i);
    }
    std::string id_at(std::size_t i) const override {
        ++reads_;
        return inner_->id_at(i);
    }
    std::optional<std::uint64_t> stored_bytes() const override {
        return inner_->stored_bytes();
    }
    void flush() override { inner_->flush(); }

    std::uint64_t reads() const { return reads_; }

private:
    std::unique_ptr<zetta::storage::StorageEngine> inner_;
    mutable std::uint64_t reads_ = 0;
};

struct DatasetListFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false},
                     &replication};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
    std::vector<const CountingEngine*> counters;

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    }

    json list(const char* detail) {
        const ApiResult listed =
            call("list_datasets", detail ? json{{"detail", detail}} : json::object());
        EXPECT_TRUE(listed.ok) << listed.error.message;
        return listed.value;
    }

    // A table of `rows` rows with three columns, in database `database`.
    std::shared_ptr<zetta::data::Table> make_table(const std::string& database,
                                                   const std::string& name,
                                                   std::size_t rows) {
        auto db = node.find_database(database);
        if (!db) db = node.create_database(database);
        EXPECT_TRUE(db);
        auto table = db->create_table(name);
        EXPECT_TRUE(table);
        for (std::size_t i = 0; i < rows; ++i) {
            table->add_row(json{{"account", "acct-" + std::to_string(i)},
                                {"plan", i % 3 == 0 ? "pro" : "free"},
                                {"seats", static_cast<int>(i % 17)}});
        }
        return table;
    }

    void flush_all() {
        for (const std::string& name : node.database_names()) {
            for (const auto& table : node.find_database(name)->tables()) {
                table->store().flush();
            }
        }
    }

    // Wrap every table's engine in a CountingEngine.
    void count_reads_everywhere() {
        for (const std::string& name : node.database_names()) {
            for (const auto& table : node.find_database(name)->tables()) {
                auto counting = std::make_unique<CountingEngine>(table->replace_store(nullptr));
                counters.push_back(counting.get());
                table->replace_store(std::move(counting));
            }
        }
    }

    std::uint64_t reads() const {
        std::uint64_t total = 0;
        for (const CountingEngine* counter : counters) total += counter->reads();
        return total;
    }
};

// Every summary entry equals its full entry on each field they share, and
// partition_count is the length of the full form's placement array.
void expect_summary_matches_full(const json& summary, const json& full) {
    ASSERT_EQ(summary.at("detail"), "summary");
    ASSERT_EQ(full.at("detail"), "full");
    const json& lean = summary.at("datasets");
    const json& whole = full.at("datasets");
    ASSERT_EQ(lean.size(), whole.size());
    for (std::size_t i = 0; i < lean.size(); ++i) {
        SCOPED_TRACE(whole[i].at("database").get<std::string>() + "." +
                     whole[i].at("name").get<std::string>());
        for (const char* field :
             {"name", "database", "table_id", "format", "source_format", "replication",
              "rows", "size_bytes", "column_order", "degraded", "degraded_reason",
              "expected_partitions", "expected_rows"}) {
            EXPECT_EQ(lean[i].at(field), whole[i].at(field)) << field;
        }
        EXPECT_EQ(lean[i].at("partition_count"), whole[i].at("partitions").size());
        // What stays on get_dataset (and the full form) is not in the summary.
        EXPECT_FALSE(lean[i].contains("columns"));
        EXPECT_FALSE(lean[i].contains("partitions"));
    }
}

// The full form is the default and keeps its documented fields; the summary form
// lists the same datasets with the same values.
TEST_F(DatasetListFixture, SummaryMatchesTheFullFormAndTheDefaultIsUnchanged) {
    make_table("crm", "accounts", 120);
    make_table("crm", "empty", 0);
    make_table("staging", "accounts", 7);  // the same name in a second database
    zetta::data::AddDatasetOptions partitioned;
    partitioned.table_name = "events";
    partitioned.partition_count = 4;
    partitioned.partition_only = true;
    std::vector<json> rows;
    for (int i = 0; i < 40; ++i) rows.push_back(json{{"id", i}, {"kind", i % 2}});
    ASSERT_TRUE(zetta::data::add_dataset_rows(node, rows, partitioned).ok());
    flush_all();

    const json full = list(nullptr);
    ASSERT_EQ(full.at("datasets").size(), 4u);
    for (const json& dataset : full.at("datasets")) {
        EXPECT_TRUE(dataset.contains("columns"));
        EXPECT_TRUE(dataset.contains("partitions"));
    }
    EXPECT_EQ(list("full"), full);
    const json summary = list("summary");
    expect_summary_matches_full(summary, full);

    bool saw_events = false;
    for (const json& dataset : summary.at("datasets")) {
        if (dataset.at("name") != "events") continue;
        saw_events = true;
        EXPECT_EQ(dataset.at("rows"), 40u);  // from the catalog's row ranges
        EXPECT_GE(dataset.at("partition_count").get<std::uint64_t>(), 2u);
        EXPECT_EQ(dataset.at("partition_count"), dataset.at("expected_partitions"));
        EXPECT_FALSE(dataset.at("degraded").get<bool>());
    }
    EXPECT_TRUE(saw_events);

    // Anything but full or summary is refused at the boundary.
    const ApiResult bad = call("list_datasets", json{{"detail", "everything"}});
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.error.code, "invalid_input");
}

// The proof the issue asks for: with 19 and with 50 datasets, small and large,
// the summary reads no row (the counting engine on every table sees none, where
// the full form reads every row) and no file (with the chunk files deleted it
// still reports the bytes they held, where the full form, which lists them,
// finds 0). Its work is per dataset, not per row.
TEST_F(DatasetListFixture, SummaryReadsNoRowAndNoFileAt19And50Datasets) {
    std::uint64_t total_rows = 0;
    for (const std::size_t datasets : {std::size_t{19}, std::size_t{50}}) {
        SCOPED_TRACE(std::to_string(datasets) + " datasets");
        while (node.find_database("data") == nullptr ||
               node.find_database("data")->tables().size() < datasets) {
            const std::size_t index =
                node.find_database("data") ? node.find_database("data")->tables().size() : 0;
            // One large table among the small ones.
            const std::size_t rows = index == 3 ? 25'000 : 20 + index;
            make_table("data", "t" + std::to_string(index), rows);
            total_rows += rows;
        }
        flush_all();
        counters.clear();
        count_reads_everywhere();

        const json summary = list("summary");
        EXPECT_EQ(reads(), 0u) << "the summary read row data";
        ASSERT_EQ(summary.at("datasets").size(), datasets);

        const json full = list("full");
        EXPECT_GE(reads(), total_rows) << "the counting engine must see the full form's scan";
        expect_summary_matches_full(summary, full);
    }

    // No file: delete every chunk file. The full form lists them and now finds
    // 0 bytes; the summary, which never lists them, still reports their size.
    const json before = list("summary");
    for (const auto& table : node.find_database("data")->tables()) {
        for (const auto& entry : fs::directory_iterator(table->dir())) {
            if (entry.path().extension() == ".jsonl") fs::remove(entry.path());
        }
    }
    const json after = list("summary");
    EXPECT_EQ(after, before);
    std::uint64_t summary_bytes = 0;
    for (const json& dataset : after.at("datasets")) {
        summary_bytes += dataset.at("size_bytes").get<std::uint64_t>();
    }
    EXPECT_GT(summary_bytes, 0u);
    for (const json& dataset : list("full").at("datasets")) {
        EXPECT_EQ(dataset.at("size_bytes"), 0u) << "the full form measures the files";
    }
}

// The counters follow every change: inserts, an update (counted when made,
// equal to the file after the flush), a deletion, a dropped table, and a
// partition-only dataset that loses a catalog entry and turns degraded.
TEST_F(DatasetListFixture, SummaryCountersFollowMutationsAndDegradedDatasets) {
    auto accounts = make_table("crm", "accounts", 30);
    make_table("crm", "scratch", 5);
    zetta::data::AddDatasetOptions partitioned;
    partitioned.table_name = "cached";
    partitioned.partition_count = 4;
    partitioned.partition_only = true;
    std::vector<json> rows;
    for (int i = 0; i < 40; ++i) rows.push_back(json{{"id", i}, {"amount", i * 3}});
    const auto added = zetta::data::add_dataset_rows(node, rows, partitioned);
    ASSERT_TRUE(added.ok()) << added.error;
    flush_all();
    expect_summary_matches_full(list("summary"), list("full"));

    accounts->add_row(json{{"account", "acct-new"}, {"plan", "enterprise"}, {"seats", 900}});
    accounts->update_row(accounts->store().id_at(0),
                         json{{"account", "acct-0"}, {"plan", "a longer plan name"}});
    accounts->delete_row(accounts->store().id_at(5));
    flush_all();
    expect_summary_matches_full(list("summary"), list("full"));

    ASSERT_TRUE(node.find_database("crm")->drop_table("scratch"));
    expect_summary_matches_full(list("summary"), list("full"));

    // Drop one catalog entry: the manifest still says 4, so the dataset is
    // degraded and its rows are a fraction, the same in both forms.
    std::string dropped;
    for (const auto& partition : node.dataset_partitions()) {
        if (partition && partition->table() == added.table->id()) {
            dropped = partition->id();
            break;
        }
    }
    ASSERT_FALSE(dropped.empty());
    ASSERT_TRUE(node.drop_dataset_partition(dropped));
    const json summary = list("summary");
    expect_summary_matches_full(summary, list("full"));
    bool saw = false;
    for (const json& dataset : summary.at("datasets")) {
        if (dataset.at("name") != "cached") continue;
        saw = true;
        EXPECT_TRUE(dataset.at("degraded").get<bool>());
        EXPECT_FALSE(dataset.at("degraded_reason").get<std::string>().empty());
        EXPECT_EQ(dataset.at("expected_partitions"), 4u);
        EXPECT_EQ(dataset.at("expected_rows"), 40u);
        EXPECT_EQ(dataset.at("partition_count"), 3u);
        EXPECT_LT(dataset.at("rows").get<std::uint64_t>(), 40u);
    }
    EXPECT_TRUE(saw);
}

}  // namespace
