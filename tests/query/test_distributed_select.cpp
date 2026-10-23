#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/query/distributed_select.hpp"
#include "zetta/query/executor.hpp"
#include "zetta/query/postgres.hpp"
#include "zetta/query/predicate.hpp"
#include "zetta/query/service.hpp"

// #833 section 5 — **scatter-gather SELECT**. A partition-only table's rows live
// in the partition bodies their owners hold, so a node-local scan answers from a
// fraction of the dataset and looks healthy doing it. These tests pin the three
// properties that make the scattered answer trustworthy:
//
//   * aggregates **merge** — count/sum add, min/max reduce, and avg divides the
//     summed value by the summed row count (never the mean of the shards' means);
//   * every partition is scanned **exactly once**, even at RF > 1, where a
//     partition has several owners;
//   * an **unreachable owner fails** the query — with RF=1 there is no replica to
//     fall back on, so a short answer must never be returned as a complete one.
namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::data::AddDatasetOptions;
using zetta::data::AddDatasetResult;
using zetta::data::add_dataset_rows;
using zetta::data::ReplicatedBlobStore;
using zetta::query::AggregateExpr;
using zetta::query::AggregateFunc;
using zetta::query::AggregatePartial;
using zetta::query::GroupedItem;
using zetta::query::GroupPartial;
using zetta::query::PartitionScanRequest;
using zetta::query::PartitionScanResponse;
using zetta::query::QueryService;
using zetta::query::ScanAggregate;
using zetta::query::SelectStmt;
using zetta::test::TempDir;

// Fixed node ids, so placement (rendezvous, a pure function of the roster) is
// identical on every run — a test that asserts which node owns what must not
// depend on a random UUID.
constexpr const char* kNodeA = "11111111-1111-4111-8111-111111111111";
constexpr const char* kNodeB = "22222222-2222-4222-8222-222222222222";

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// 60 rows over 3 groups with deliberately uneven values, so a wrong `avg` (the
// mean of the shards' means) cannot coincide with the right one.
std::vector<nlohmann::json> sample_rows() {
    std::vector<nlohmann::json> rows;
    for (int i = 0; i < 60; ++i) {
        rows.push_back(nlohmann::json{{"segment", i % 3},
                                      {"amount", i * (i % 7 + 1)},
                                      {"label", "row-" + std::to_string(i)}});
    }
    return rows;
}

// The truth, computed straight from the rows.
struct GroupTruth {
    std::uint64_t count = 0;
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
    double avg() const { return count == 0 ? 0.0 : sum / static_cast<double>(count); }
};

std::map<int, GroupTruth> truth_of(const std::vector<nlohmann::json>& rows) {
    std::map<int, GroupTruth> truth;
    for (const nlohmann::json& row : rows) {
        const int segment = row.at("segment").get<int>();
        const double amount = row.at("amount").get<double>();
        GroupTruth& group = truth[segment];
        if (group.count == 0) {
            group.min = amount;
            group.max = amount;
        } else {
            group.min = std::min(group.min, amount);
            group.max = std::max(group.max, amount);
        }
        ++group.count;
        group.sum += amount;
    }
    return truth;
}

template <class Predicate>
bool wait_for(Predicate done, std::chrono::seconds budget = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

// Tick both nodes until `done` holds. Patient: a seconds-long deadline, not a
// fixed iteration count.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

// `SELECT segment, count(*), sum(amount), min(amount), max(amount), avg(amount)
//  FROM datasets.cached GROUP BY segment` — asserted against the truth.
void expect_matches_truth(const QueryService::Outcome& outcome,
                          const std::map<int, GroupTruth>& truth) {
    ASSERT_TRUE(outcome.ok()) << outcome.error;
    ASSERT_EQ(outcome.rows.size(), truth.size());
    std::size_t index = 0;
    for (const auto& [segment, group] : truth) {
        const nlohmann::json& row = outcome.rows[index++];
        EXPECT_EQ(row.at("segment").get<int>(), segment);
        EXPECT_EQ(row.at("count").get<std::uint64_t>(), group.count);
        EXPECT_DOUBLE_EQ(row.at("sum_amount").get<double>(), group.sum);
        EXPECT_DOUBLE_EQ(row.at("min_amount").get<double>(), group.min);
        EXPECT_DOUBLE_EQ(row.at("max_amount").get<double>(), group.max);
        EXPECT_DOUBLE_EQ(row.at("mean_amount").get<double>(), group.avg());
    }
}

// Parse one SELECT, for calling the scatter-gather planner directly.
SelectStmt parse_select(const std::string& sql) {
    zetta::query::PostgresDialect dialect;
    auto parsed = dialect.parse(sql);
    EXPECT_TRUE(parsed.ok()) << parsed.error;
    return std::get<SelectStmt>(parsed.statements.at(0));
}

// The full answer cut down by hand: ORDER BY (stable), OFFSET, LIMIT, exactly
// as the coordinator applies them to the merged rows.
std::vector<nlohmann::json> cut(std::vector<nlohmann::json> rows,
                                const SelectStmt& statement) {
    zetta::query::sort_rows_for_order(rows, statement.order_by);
    const std::size_t from = std::min(statement.offset, rows.size());
    rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(from));
    if (statement.limit && rows.size() > *statement.limit) rows.resize(*statement.limit);
    return rows;
}

constexpr const char* kGroupedSql =
    "SELECT segment, count(*), sum(amount), min(amount), max(amount), "
    "avg(amount) FROM datasets.cached GROUP BY segment;";

}  // namespace

// --- the merge rules, asserted directly ---------------------------------------

// Two shards of wildly different size. Averaging the averages would give
// (10 + 1) / 2 = 5.5; weighting by row count gives (10*1 + 1*99) / 100 = 1.09.
TEST(ScatterGatherMerge, AvgWeightsByRowCountRatherThanAveragingTheAverages) {
    SelectStmt statement;
    statement.table = {"datasets", "cached"};
    statement.group_by = {"segment"};
    statement.items.push_back(GroupedItem{std::nullopt, "segment", ""});
    statement.items.push_back(
        GroupedItem{AggregateExpr{AggregateFunc::Avg, "amount"}, "", ""});
    statement.items.push_back(
        GroupedItem{AggregateExpr{AggregateFunc::Count, ""}, "", ""});

    PartitionScanResponse small;
    small.from = "a";
    small.groups.push_back(GroupPartial{
        nlohmann::json(7),
        {AggregatePartial{/*value=*/10.0, /*count=*/1, /*integral=*/true},
         AggregatePartial{/*value=*/1.0, /*count=*/1, /*integral=*/true}}});

    PartitionScanResponse large;
    large.from = "b";
    large.groups.push_back(GroupPartial{
        nlohmann::json(7),
        {AggregatePartial{/*value=*/99.0, /*count=*/99, /*integral=*/true},
         AggregatePartial{/*value=*/99.0, /*count=*/99, /*integral=*/true}}});

    const zetta::query::SelectOutcome merged =
        zetta::query::merge_scan_responses(statement, {}, {small, large});
    ASSERT_TRUE(merged.ok()) << merged.error;
    ASSERT_EQ(merged.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(merged.rows[0].at("mean_amount").get<double>(), 109.0 / 100.0);
    EXPECT_EQ(merged.rows[0].at("count").get<std::uint64_t>(), 100u);
    EXPECT_EQ(merged.rows[0].at("segment").get<int>(), 7);
}

// count/sum add; min/max reduce; a shard that contributed nothing to an
// aggregate leaves its identity alone rather than dragging min to zero.
TEST(ScatterGatherMerge, CountAndSumAddWhileMinAndMaxReduce) {
    SelectStmt statement;
    statement.table = {"datasets", "cached"};
    statement.group_by = {"segment"};
    statement.items.push_back(GroupedItem{std::nullopt, "segment", ""});
    statement.items.push_back(
        GroupedItem{AggregateExpr{AggregateFunc::Sum, "amount"}, "", ""});
    statement.items.push_back(
        GroupedItem{AggregateExpr{AggregateFunc::Min, "amount"}, "", ""});
    statement.items.push_back(
        GroupedItem{AggregateExpr{AggregateFunc::Max, "amount"}, "", ""});

    PartitionScanResponse first;
    first.from = "a";
    first.groups.push_back(GroupPartial{nlohmann::json(1),
                                        {AggregatePartial{30.0, 3, true},
                                         AggregatePartial{5.0, 3, true},
                                         AggregatePartial{20.0, 3, true}}});
    PartitionScanResponse second;
    second.from = "b";
    second.groups.push_back(GroupPartial{nlohmann::json(1),
                                         {AggregatePartial{12.0, 2, true},
                                          AggregatePartial{2.0, 2, true},
                                          AggregatePartial{9.0, 2, true}}});
    // A shard whose rows all had a null amount: it saw rows, but no VALUES.
    PartitionScanResponse empty;
    empty.from = "c";
    empty.groups.push_back(GroupPartial{nlohmann::json(1),
                                        {AggregatePartial{0.0, 0, true},
                                         AggregatePartial{0.0, 0, true},
                                         AggregatePartial{0.0, 0, true}}});

    const zetta::query::SelectOutcome merged =
        zetta::query::merge_scan_responses(statement, {}, {first, second, empty});
    ASSERT_EQ(merged.rows.size(), 1u);
    EXPECT_EQ(merged.rows[0].at("sum_amount").get<std::int64_t>(), 42);
    EXPECT_EQ(merged.rows[0].at("min_amount").get<std::int64_t>(), 2);
    EXPECT_EQ(merged.rows[0].at("max_amount").get<std::int64_t>(), 20);
}

// --- the wire boundary ---------------------------------------------------------

TEST(ScatterGatherWire, RequestAndResponseRoundTripThroughTheirSchemas) {
    PartitionScanRequest request;
    request.table = "tbl-1";
    request.partitions = {"part-a", "part-b"};
    request.where = zetta::query::WhereClause{"segment", zetta::query::CompareOp::Greater,
                                              nlohmann::json(3)};
    request.group_by = "segment";
    request.aggregates = {ScanAggregate{AggregateFunc::Avg, "amount"},
                          ScanAggregate{AggregateFunc::Count, ""}};

    std::string error;
    const std::optional<PartitionScanRequest> decoded =
        zetta::query::decode_partition_scan_request(
            zetta::query::encode_partition_scan_request(request), &error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->table, request.table);
    EXPECT_EQ(decoded->partitions, request.partitions);
    ASSERT_TRUE(decoded->where);
    EXPECT_EQ(decoded->where->column, "segment");
    EXPECT_EQ(decoded->where->value, nlohmann::json(3));
    EXPECT_EQ(decoded->group_by, "segment");
    ASSERT_EQ(decoded->aggregates.size(), 2u);
    EXPECT_EQ(decoded->aggregates[0].column, "amount");

    PartitionScanResponse response;
    response.from = "node-a";
    response.partitions_read = 2;
    response.rows_scanned = 17;
    response.groups.push_back(
        GroupPartial{nlohmann::json(2), {AggregatePartial{6.0, 4, false}}});
    const std::optional<PartitionScanResponse> back =
        zetta::query::decode_partition_scan_response(
            zetta::query::encode_partition_scan_response(response), &error);
    ASSERT_TRUE(back) << error;
    EXPECT_EQ(back->from, "node-a");
    EXPECT_EQ(back->rows_scanned, 17u);
    ASSERT_EQ(back->groups.size(), 1u);
    EXPECT_EQ(back->groups[0].aggregates[0].count, 4u);
    EXPECT_FALSE(back->groups[0].aggregates[0].integral);
    EXPECT_FALSE(back->columns_seen);  // not reported -> not checked

    // A whole-table aggregate: aggregates with no group key (#1218).
    PartitionScanRequest whole;
    whole.table = "tbl:x";
    whole.aggregates = {ScanAggregate{AggregateFunc::Count, ""}};
    const std::optional<PartitionScanRequest> whole_back =
        zetta::query::decode_partition_scan_request(
            zetta::query::encode_partition_scan_request(whole), &error);
    ASSERT_TRUE(whole_back) << error;
    EXPECT_TRUE(whole_back->aggregating());
    EXPECT_TRUE(whole_back->group_by.empty());
    ASSERT_EQ(whole_back->aggregates.size(), 1u);

    // LIMIT pushdown (#1021): the per-partition cap and its ORDER BY travel.
    PartitionScanRequest limited;
    limited.table = "tbl:x";
    limited.partitions = {"p"};
    limited.keep = 7;
    limited.order_by = {zetta::query::OrderKey{"amount", true},
                        zetta::query::OrderKey{"label", false}};
    const std::optional<PartitionScanRequest> limited_back =
        zetta::query::decode_partition_scan_request(
            zetta::query::encode_partition_scan_request(limited), &error);
    ASSERT_TRUE(limited_back) << error;
    EXPECT_EQ(limited_back->keep, 7u);
    ASSERT_EQ(limited_back->order_by.size(), 2u);
    EXPECT_EQ(limited_back->order_by[0].column, "amount");
    EXPECT_TRUE(limited_back->order_by[0].descending);
    EXPECT_FALSE(limited_back->order_by[1].descending);
    EXPECT_FALSE(whole_back->keep) << "no LIMIT, no cap";

    // The columns an owner's rows carry travel with its answer.
    response.columns_seen = std::vector<std::string>{"segment", "amount"};
    const std::optional<PartitionScanResponse> with_columns =
        zetta::query::decode_partition_scan_response(
            zetta::query::encode_partition_scan_response(response), &error);
    ASSERT_TRUE(with_columns) << error;
    ASSERT_TRUE(with_columns->columns_seen);
    EXPECT_EQ(*with_columns->columns_seen,
              (std::vector<std::string>{"segment", "amount"}));
}

// #912: a compound WHERE travels to the owner whole: AND / OR / NOT nest, and the
// owner's decoded predicate matches exactly the rows the coordinator's would.
TEST(ScatterGatherWire, ACompoundWhereRoundTripsToTheOwner) {
    using zetta::query::CompareOp;
    using zetta::query::WhereClause;
    WhereClause pro{"plan", CompareOp::Equal, nlohmann::json("pro")};
    WhereClause big{"amount", CompareOp::Greater, nlohmann::json(5)};
    WhereClause free_plan{"plan", CompareOp::Equal, nlohmann::json("free")};
    WhereClause both;
    both.kind = WhereClause::Kind::And;
    both.operands = {pro, big};
    WhereClause not_free;
    not_free.kind = WhereClause::Kind::Not;
    not_free.operands = {free_plan};
    WhereClause either;
    either.kind = WhereClause::Kind::Or;
    either.operands = {both, not_free};

    PartitionScanRequest request;
    request.table = "tbl-1";
    request.partitions = {"part-a"};
    request.where = either;
    std::string error;
    const std::optional<PartitionScanRequest> decoded =
        zetta::query::decode_partition_scan_request(
            zetta::query::encode_partition_scan_request(request), &error);
    ASSERT_TRUE(decoded) << error;
    ASSERT_TRUE(decoded->where);
    ASSERT_EQ(decoded->where->kind, WhereClause::Kind::Or);
    ASSERT_EQ(decoded->where->operands.size(), 2u);
    EXPECT_EQ(decoded->where->operands[0].kind, WhereClause::Kind::And);
    EXPECT_EQ(decoded->where->operands[0].operands[1].column, "amount");
    EXPECT_EQ(decoded->where->operands[1].kind, WhereClause::Kind::Not);
    EXPECT_EQ(decoded->where->operands[1].operands[0].value, nlohmann::json("free"));

    for (const nlohmann::json& row :
         {nlohmann::json{{"plan", "pro"}, {"amount", 9}},
          nlohmann::json{{"plan", "pro"}, {"amount", 1}},
          nlohmann::json{{"plan", "free"}, {"amount", 9}},
          nlohmann::json{{"plan", "team"}, {"amount", 0}},
          nlohmann::json{{"amount", 0}}}) {
        SCOPED_TRACE(row.dump());
        EXPECT_EQ(zetta::query::row_matches(row, decoded->where),
                  zetta::query::row_matches(row, request.where));
    }

    // A compound with too few operands is refused at the boundary.
    EXPECT_FALSE(zetta::query::decode_partition_scan_request(
        R"({"table":"t","partitions":[],"where":{"and":[{"column":"a","op":"eq","value":1}]}})",
        &error));
    EXPECT_FALSE(zetta::query::decode_partition_scan_request(
        R"({"table":"t","partitions":[],"where":{"not":{"column":"a","op":"like","value":1}}})",
        &error));
}

TEST(ScatterGatherWire, MalformedMessagesAreRejectedAtTheBoundary) {
    std::string error;
    EXPECT_FALSE(zetta::query::decode_partition_scan_request("{", &error));
    EXPECT_FALSE(error.empty());
    // No table.
    EXPECT_FALSE(zetta::query::decode_partition_scan_request(
        R"({"partitions":["p"]})", &error));
    // An unknown comparison.
    EXPECT_FALSE(zetta::query::decode_partition_scan_request(
        R"({"table":"t","partitions":[],"where":{"column":"a","op":"like","value":1}})",
        &error));
    // A negative cap is refused (#1021).
    EXPECT_FALSE(zetta::query::decode_partition_scan_request(
        R"({"table":"t","partitions":[],"keep":-1})", &error));
    // A partial with no row count is not a partial.
    EXPECT_FALSE(zetta::query::decode_partition_scan_response(
        R"({"from":"n","groups":[{"key":1,"aggregates":[{"value":2.0}]}]})", &error));
}

// --- single node: the reference the multi-node answer must equal ---------------

TEST(ScatterGatherSelect, SingleNodeAnswersAPartitionOnlyTableFromItsPartitions) {
    TempDir dir;
    Node node{config_for(dir)};
    node.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 8;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;

    QueryService service{node};
    expect_matches_truth(service.run(kGroupedSql, ""), truth_of(sample_rows()));

    // A plain SELECT returns every row exactly once, even though the local store
    // also happens to hold a staging copy of them.
    const auto all = service.run("SELECT * FROM datasets.cached;", "");
    ASSERT_TRUE(all.ok()) << all.error;
    EXPECT_EQ(all.rows.size(), 60u);
    std::set<std::string> labels;
    for (const nlohmann::json& row : all.rows) {
        labels.insert(row.at("label").get<std::string>());
    }
    EXPECT_EQ(labels.size(), 60u);

    // WHERE is pushed to the owner; the projection comes back projected.
    const auto filtered =
        service.run("SELECT label FROM datasets.cached WHERE segment = 1;", "");
    ASSERT_TRUE(filtered.ok()) << filtered.error;
    EXPECT_EQ(filtered.rows.size(), 20u);
    EXPECT_TRUE(filtered.rows[0].contains("label"));
    EXPECT_FALSE(filtered.rows[0].contains("segment"));

    // #912: a compound WHERE is pushed to the owners too, and answers like the
    // rows filtered by hand.
    const auto compound = service.run(
        "SELECT label FROM datasets.cached WHERE segment = 1 AND amount > 100 "
        "OR NOT segment <> 2 AND amount < 10;",
        "");
    ASSERT_TRUE(compound.ok()) << compound.error;
    std::size_t expected = 0;
    for (const nlohmann::json& row : sample_rows()) {
        const int segment = row.at("segment").get<int>();
        const int amount = row.at("amount").get<int>();
        if ((segment == 1 && amount > 100) || (segment == 2 && amount < 10)) ++expected;
    }
    ASSERT_GT(expected, 0u);
    EXPECT_EQ(compound.rows.size(), expected);

    node.stop();
}

// #1021: LIMIT is pushed to the partitions. No partition sends more than
// offset + limit rows (with ORDER BY, its own top rows by the same stable sort),
// and the merged answer equals the full scan cut down by hand.
TEST(ScatterGatherSelect, LimitIsPushedDownAndAnswersLikeTheFullScan) {
    TempDir dir;
    Node node{config_for(dir)};
    node.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 8;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    ASSERT_EQ(added.partitions.size(), 8u);
    const std::string table_id = added.table->id();

    const zetta::query::SelectOutcome full = zetta::query::select_over_partitions(
        node, nullptr, parse_select("SELECT * FROM datasets.cached;"), table_id);
    ASSERT_TRUE(full.ok()) << full.error;
    ASSERT_EQ(full.rows.size(), 60u);
    EXPECT_EQ(full.rows_received, 60u);

    QueryService service{node};
    for (const std::string sql :
         {"SELECT * FROM datasets.cached LIMIT 5;",
          "SELECT * FROM datasets.cached OFFSET 3 LIMIT 4;",
          "SELECT label FROM datasets.cached FETCH FIRST 2 ROWS ONLY;",
          "SELECT * FROM datasets.cached ORDER BY amount DESC LIMIT 4;",
          "SELECT * FROM datasets.cached ORDER BY segment, amount DESC OFFSET 2 LIMIT 5;",
          "SELECT label, amount FROM datasets.cached WHERE segment = 2 "
          "ORDER BY amount LIMIT 3;",
          "SELECT * FROM datasets.cached ORDER BY segment LIMIT 7;"}) {
        SCOPED_TRACE(sql);
        const SelectStmt statement = parse_select(sql);
        const zetta::query::SelectOutcome scanned =
            zetta::query::select_over_partitions(node, nullptr, statement, table_id);
        ASSERT_TRUE(scanned.ok()) << scanned.error;
        const std::size_t keep = statement.offset + *statement.limit;
        EXPECT_LE(scanned.rows_received, keep * added.partitions.size())
            << "a partition sent more than offset + limit rows";
        EXPECT_LT(scanned.rows_received, 60u) << "the LIMIT did not reach the partitions";

        // The answer through the executor equals the full scan, filtered,
        // projected and cut by hand.
        SelectStmt unlimited = statement;
        unlimited.limit.reset();
        unlimited.offset = 0;
        unlimited.order_by.clear();
        const zetta::query::SelectOutcome everything =
            zetta::query::select_over_partitions(node, nullptr, unlimited, table_id);
        ASSERT_TRUE(everything.ok()) << everything.error;
        const auto answered = service.run(sql, "");
        ASSERT_TRUE(answered.ok()) << answered.error;
        EXPECT_EQ(answered.rows, cut(everything.rows, statement));
    }

    // An aggregate is not cut at the partitions: every row still counts.
    const auto counted = service.run("SELECT count(*) FROM datasets.cached LIMIT 1;", "");
    ASSERT_TRUE(counted.ok()) << counted.error;
    EXPECT_EQ(counted.rows.at(0).at("count"), 60);
    // An ORDER BY key the result lacks is still refused under LIMIT 0.
    EXPECT_FALSE(service.run("SELECT label FROM datasets.cached ORDER BY amount LIMIT 0;", "").ok());

    node.stop();
}

// #1021: ORDER BY on a column the caller may not read is refused, as WHERE and
// GROUP BY on one are: with LIMIT pushed to the owners, the sort would choose
// which rows are kept (and their order) by values the caller cannot see. Over a
// partition-only table and a replicated one alike; an ORDER BY on a readable
// column still pushes down and answers like the full scan, the hidden column
// masked.
TEST(ScatterGatherSelect, AnOrderByOnAMaskedColumnIsRefused) {
    TempDir dir;
    Node node{config_for(dir)};
    node.start();
    node.ensure_builtin_roles();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    const auto replicated = node.find_database("datasets")->create_table("plain");
    ASSERT_TRUE(replicated);
    for (const nlohmann::json& row : sample_rows()) replicated->add_row(row);

    ASSERT_TRUE(node.create_role("clerk"));
    for (const char* table : {"cached", "plain"}) {
        ASSERT_TRUE(node.grant_scoped_permissions(
            "clerk", {"sql:read"},
            zetta::governance::Resource::column("datasets", table, {"segment", "label"})));
    }
    const auto user = node.create_user("cleo");
    ASSERT_TRUE(user);
    ASSERT_TRUE(node.assign_role(user->id(), node.find_role("clerk")->id()));

    const auto run_as = [&](const std::string& sql, const std::string& acting,
                            bool is_user) {
        zetta::query::PostgresDialect dialect;
        auto parsed = dialect.parse(sql);
        EXPECT_TRUE(parsed.ok()) << parsed.error;
        zetta::query::Executor executor{node, nullptr, acting, is_user};
        return executor.execute(parsed.statements.at(0), "");
    };

    for (const std::string table : {"cached", "plain"}) {
        for (const std::string tail :
             {" ORDER BY amount DESC LIMIT 5;", " ORDER BY amount DESC;",
              " ORDER BY segment, amount DESC LIMIT 7;"}) {
            const std::string sql = "SELECT * FROM datasets." + table + tail;
            SCOPED_TRACE(sql);
            const auto refused = run_as(sql, user->id(), true);
            ASSERT_FALSE(refused.ok());
            EXPECT_NE(refused.error.find("permission denied: no sql:read grant on column "
                                         "\"amount\" used in ORDER BY"),
                      std::string::npos)
                << refused.error;
        }
    }

    // ORDER BY a readable column: pushed down, the same rows as the unlimited
    // answer cut by hand, amount masked.
    const auto everything =
        run_as("SELECT * FROM datasets.cached ORDER BY segment DESC;", user->id(), true);
    ASSERT_TRUE(everything.ok()) << everything.error;
    ASSERT_EQ(everything.rows.size(), 60u);
    const auto limited = run_as(
        "SELECT * FROM datasets.cached ORDER BY segment DESC OFFSET 2 LIMIT 5;", user->id(),
        true);
    ASSERT_TRUE(limited.ok()) << limited.error;
    SelectStmt statement =
        parse_select("SELECT * FROM datasets.cached ORDER BY segment DESC OFFSET 2 LIMIT 5;");
    statement.order_by.clear();  // `everything` is already in order
    EXPECT_EQ(limited.rows, cut(everything.rows, statement));
    for (const nlohmann::json& row : limited.rows) {
        EXPECT_TRUE(row.at("amount").is_null()) << "amount is masked";
    }

    // A caller who may read amount still gets the owners' top rows by it.
    const auto local = run_as("SELECT * FROM datasets.cached ORDER BY amount DESC LIMIT 1;",
                              "", false);
    ASSERT_TRUE(local.ok()) << local.error;
    ASSERT_EQ(local.rows.size(), 1u);
    int highest = 0;
    for (const nlohmann::json& row : sample_rows()) {
        highest = std::max(highest, row.at("amount").get<int>());
    }
    EXPECT_EQ(local.rows[0].at("amount"), highest);

    node.stop();
}

// #1021: a dataset loaded with a column order (a connector query's SELECT list)
// keeps it — SELECT * over the partitions and the dataset summary list the
// columns that way — and its columns are typed from the partitions' stats.
TEST(ScatterGatherSelect, APartitionOnlyDatasetKeepsItsSourceColumnOrder) {
    TempDir dir;
    Node node{config_for(dir)};
    node.start();

    std::vector<nlohmann::json> rows = sample_rows();
    rows[0]["label"] = nullptr;
    rows[1].erase("label");
    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.partition_only = true;
    options.columns = {"segment", "label", "amount"};
    const AddDatasetResult added = add_dataset_rows(node, rows, options);
    ASSERT_TRUE(added.ok()) << added.error;

    QueryService service{node};
    const auto star = service.run("SELECT * FROM datasets.cached LIMIT 1;", "");
    ASSERT_TRUE(star.ok()) << star.error;
    EXPECT_EQ(star.columns, (std::vector<std::string>{"segment", "label", "amount"}));

    bool found = false;
    for (const zetta::data::DatasetSummary& dataset : node.datasets()) {
        if (dataset.table_id != added.table->id()) continue;
        found = true;
        EXPECT_TRUE(dataset.column_order_recorded);
        EXPECT_EQ(dataset.columns, (std::vector<std::string>{"segment", "label", "amount"}));
        ASSERT_EQ(dataset.typed_columns.size(), 3u);
        EXPECT_EQ(dataset.typed_columns[0].type, "integer");
        EXPECT_FALSE(dataset.typed_columns[0].nullable);
        EXPECT_EQ(dataset.typed_columns[1].type, "string");
        EXPECT_TRUE(dataset.typed_columns[1].nullable);
        ASSERT_TRUE(dataset.typed_columns[1].null_fraction);
        EXPECT_DOUBLE_EQ(*dataset.typed_columns[1].null_fraction, 2.0 / 60.0);
        EXPECT_EQ(dataset.typed_columns[2].type, "integer");
    }
    EXPECT_TRUE(found);
    node.stop();
}

// #1218 over a partition-only table: a whole-table aggregate, a text GROUP BY
// key, an unknown column and SELECT *'s columns all answer through the owners'
// scan exactly as they do over a replicated table.
TEST(ScatterGatherSelect, WholeTableAggregatesTextKeysAndColumnsOverPartitions) {
    TempDir dir;
    Node node{config_for(dir)};
    node.start();

    std::vector<nlohmann::json> rows;
    for (int i = 0; i < 30; ++i) {
        rows.push_back(nlohmann::json{{"plan", i % 3 == 0 ? "pro" : "free"},
                                      {"amount", i}});
    }
    AddDatasetOptions options;
    options.table_name = "plans";
    options.partition_count = 4;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, rows, options);
    ASSERT_TRUE(added.ok()) << added.error;

    QueryService service{node};
    const auto counted = service.run("SELECT count(*) FROM datasets.plans;", "");
    ASSERT_TRUE(counted.ok()) << counted.error;
    ASSERT_EQ(counted.rows.size(), 1u);
    EXPECT_EQ(counted.rows[0].at("count"), 30);

    const auto none =
        service.run("SELECT count(*) FROM datasets.plans WHERE amount > 100;", "");
    ASSERT_TRUE(none.ok()) << none.error;
    ASSERT_EQ(none.rows.size(), 1u);
    EXPECT_EQ(none.rows[0].at("count"), 0);

    const auto grouped = service.run(
        "SELECT plan, count(*), sum(amount) FROM datasets.plans GROUP BY plan;", "");
    ASSERT_TRUE(grouped.ok()) << grouped.error;
    ASSERT_EQ(grouped.rows.size(), 2u);
    EXPECT_EQ(grouped.rows[0].at("plan"), "free");
    EXPECT_EQ(grouped.rows[0].at("count"), 20);
    EXPECT_EQ(grouped.rows[1].at("plan"), "pro");
    EXPECT_EQ(grouped.rows[1].at("count"), 10);
    EXPECT_EQ(grouped.rows[1].at("sum_amount"), 135);  // 0 + 3 + ... + 27

    const auto unknown = service.run("SELECT nope FROM datasets.plans;", "");
    EXPECT_FALSE(unknown.ok());
    EXPECT_NE(unknown.error.find("column \"nope\" does not exist"), std::string::npos)
        << unknown.error;

    const auto star = service.run("SELECT * FROM datasets.plans LIMIT 3;", "");
    ASSERT_TRUE(star.ok()) << star.error;
    EXPECT_EQ(star.rows.size(), 3u);
    EXPECT_EQ(std::set<std::string>(star.columns.begin(), star.columns.end()),
              (std::set<std::string>{"plan", "amount"}));

    node.stop();
}

// A whole-table aggregate with nothing to merge is still one row.
TEST(ScatterGatherMerge, WholeTableAggregateOverNoGroupsIsOneRow) {
    SelectStmt statement;
    statement.table = {"datasets", "cached"};
    statement.items.push_back(
        GroupedItem{AggregateExpr{AggregateFunc::Count, ""}, "", ""});
    statement.items.push_back(
        GroupedItem{AggregateExpr{AggregateFunc::Sum, "amount"}, "", ""});

    const zetta::query::SelectOutcome merged =
        zetta::query::merge_scan_responses(statement, {}, {});
    ASSERT_TRUE(merged.ok()) << merged.error;
    ASSERT_EQ(merged.rows.size(), 1u);
    EXPECT_EQ(merged.rows[0].at("count"), 0);
    EXPECT_TRUE(merged.rows[0].at("sum_amount").is_null());
}

// --- two nodes: the same answer, from partitions split across them --------------

TEST(ScatterGatherSelect, MultiNodeAggregatesMatchTheSingleNodeAnswer) {
    TempDir dir_a;
    TempDir dir_b;
    ReplicationManager repl_a{kNodeA};
    ReplicationManager repl_b{kNodeB};
    // The partition bodies travel peer-to-peer over the large-object store, so
    // the clusters need one — a node without it never receives what it owns.
    ReplicatedBlobStore blobs_a{dir_a.path(), repl_a};
    ReplicatedBlobStore blobs_b{dir_b.path(), repl_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43710;
    cfg_a.port_range = 1;
    Cluster cluster_a{kNodeA, cfg_a, &repl_a, &blobs_a};
    cluster_a.start();
    a.attach_cluster(cluster_a);

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43711;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43710"};
    Cluster cluster_b{kNodeB, cfg_b, &repl_b, &blobs_b};
    cluster_b.start();
    b.attach_cluster(cluster_b);

    ASSERT_TRUE(wait_for([&] {
        return cluster_a.peer_count() == 1 && cluster_b.peer_count() == 1;
    }));

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 8;
    options.replication_factor = 1;  // one owner each, so the scan must split
    options.partition_only = true;
    options.members = {kNodeA, kNodeB};
    options.columns = {"label", "segment", "amount"};  // the source's order (#1021)
    const AddDatasetResult added = add_dataset_rows(a, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;

    // Placement must actually have split, or the test proves nothing.
    std::set<std::string> owners;
    for (const auto& partition : added.partitions) {
        for (const std::string& owner : partition->owner_nodes()) owners.insert(owner);
    }
    ASSERT_EQ(owners.size(), 2u) << "rendezvous placed every partition on one node";

    // Wait for the bodies to reach their owners on B and for both snapshots to
    // catch up (the ticks in converge drive the refresh).
    const bool ready = converge(a, b, [&] {
        return b.dataset_partition_count() == added.partitions.size();
    });
    ASSERT_TRUE(ready);

    QueryService service_a{a};
    const auto truth = truth_of(sample_rows());
    // The bodies travel peer-to-peer; retry patiently until B holds the ones it
    // owns, then the scattered answer must be exact.
    QueryService::Outcome grouped;
    const bool answered = converge(a, b, [&] {
        grouped = service_a.run(kGroupedSql, "");
        return grouped.ok();
    });
    ASSERT_TRUE(answered) << grouped.error;
    expect_matches_truth(grouped, truth);

    // The peer really answered for what it owns.
    ASSERT_TRUE(b.distributed_select());
    EXPECT_GT(b.distributed_select()->scans_served(), 0u);

    // Every node is a valid coordinator and gets the identical answer.
    QueryService service_b{b};
    expect_matches_truth(service_b.run(kGroupedSql, ""), truth);

    // And a plain SELECT still returns each row exactly once across the split.
    const auto all = service_a.run("SELECT * FROM datasets.cached;", "");
    ASSERT_TRUE(all.ok()) << all.error;
    std::set<std::string> labels;
    for (const nlohmann::json& row : all.rows) {
        labels.insert(row.at("label").get<std::string>());
    }
    EXPECT_EQ(all.rows.size(), 60u);
    EXPECT_EQ(labels.size(), 60u);
    // SELECT * names its columns from what the owners' rows carry (#1218), in
    // the order the load recorded (#1021).
    EXPECT_EQ(all.columns, (std::vector<std::string>{"label", "segment", "amount"}));

    // The order replicated (`cols:<table-id>`): B, which did not load the
    // dataset, lists the same columns in the same order.
    ASSERT_TRUE(converge(a, b, [&] {
        for (const zetta::data::DatasetSummary& dataset : b.datasets()) {
            if (dataset.table_id == added.table->id()) return dataset.column_order_recorded;
        }
        return false;
    }));
    for (const zetta::data::DatasetSummary& dataset : b.datasets()) {
        if (dataset.table_id != added.table->id()) continue;
        EXPECT_EQ(dataset.columns, (std::vector<std::string>{"label", "segment", "amount"}));
    }

    // LIMIT across the split (#1021): each owner sends at most offset + limit
    // rows per partition, and either coordinator answers what the full scan
    // cut by hand would.
    for (Node* coordinator : {&a, &b}) {
        for (const std::string sql :
             {"SELECT * FROM datasets.cached LIMIT 3;",
              "SELECT label, amount FROM datasets.cached ORDER BY amount DESC OFFSET 1 LIMIT 4;"}) {
            SCOPED_TRACE(sql);
            const SelectStmt statement = parse_select(sql);
            ASSERT_TRUE(coordinator->distributed_select());
            const zetta::query::SelectOutcome scanned =
                coordinator->distributed_select()->execute(statement, added.table->id());
            ASSERT_TRUE(scanned.ok()) << scanned.error;
            EXPECT_LE(scanned.rows_received,
                      (statement.offset + *statement.limit) * added.partitions.size());
            EXPECT_LT(scanned.rows_received, 60u);
            SelectStmt unlimited = statement;
            unlimited.limit.reset();
            unlimited.offset = 0;
            unlimited.order_by.clear();
            const zetta::query::SelectOutcome everything =
                coordinator->distributed_select()->execute(unlimited, added.table->id());
            ASSERT_TRUE(everything.ok()) << everything.error;
            QueryService service{*coordinator};
            const auto answered = service.run(sql, "");
            ASSERT_TRUE(answered.ok()) << answered.error;
            EXPECT_EQ(answered.rows, cut(everything.rows, statement));
        }
    }

    // A whole-table aggregate and a text GROUP BY key merge across the split,
    // and a column no owner saw is an error on either coordinator (#1218).
    for (QueryService* service : {&service_a, &service_b}) {
        const auto counted =
            service->run("SELECT count(*) FROM datasets.cached WHERE segment = 1;", "");
        ASSERT_TRUE(counted.ok()) << counted.error;
        ASSERT_EQ(counted.rows.size(), 1u);
        EXPECT_EQ(counted.rows[0].at("count"), 20);

        const auto by_label = service->run(
            "SELECT label, count(*) FROM datasets.cached GROUP BY label;", "");
        ASSERT_TRUE(by_label.ok()) << by_label.error;
        EXPECT_EQ(by_label.rows.size(), 60u);
        EXPECT_EQ(by_label.rows[0].at("count"), 1);

        const auto unknown = service->run("SELECT nope FROM datasets.cached;", "");
        EXPECT_FALSE(unknown.ok());
        EXPECT_NE(unknown.error.find("column \"nope\" does not exist"),
                  std::string::npos)
            << unknown.error;
    }

    b.stop();
    a.stop();
    cluster_b.stop();
    cluster_a.stop();
}

// A partition with two owners must still be read once — the whole reason the
// coordinator assigns responders instead of letting every owner volunteer.
TEST(ScatterGatherSelect, ReplicatedPartitionsAreScannedExactlyOnce) {
    TempDir dir_a;
    TempDir dir_b;
    ReplicationManager repl_a{kNodeA};
    ReplicationManager repl_b{kNodeB};
    // The partition bodies travel peer-to-peer over the large-object store, so
    // the clusters need one — a node without it never receives what it owns.
    ReplicatedBlobStore blobs_a{dir_a.path(), repl_a};
    ReplicatedBlobStore blobs_b{dir_b.path(), repl_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43712;
    cfg_a.port_range = 1;
    Cluster cluster_a{kNodeA, cfg_a, &repl_a, &blobs_a};
    cluster_a.start();
    a.attach_cluster(cluster_a);

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43713;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43712"};
    Cluster cluster_b{kNodeB, cfg_b, &repl_b, &blobs_b};
    cluster_b.start();
    b.attach_cluster(cluster_b);

    ASSERT_TRUE(wait_for([&] {
        return cluster_a.peer_count() == 1 && cluster_b.peer_count() == 1;
    }));

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.replication_factor = 2;  // every partition on BOTH nodes
    options.partition_only = true;
    options.members = {kNodeA, kNodeB};
    const AddDatasetResult added = add_dataset_rows(a, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    for (const auto& partition : added.partitions) {
        ASSERT_EQ(partition->owner_nodes().size(), 2u);
    }

    ASSERT_TRUE(converge(a, b, [&] {
        return b.dataset_partition_count() == added.partitions.size();
    }));

    QueryService service{a};
    expect_matches_truth(service.run(kGroupedSql, ""), truth_of(sample_rows()));

    b.stop();
    a.stop();
    cluster_b.stop();
    cluster_a.stop();
}

// The one outcome that must never ship: a short answer that looks complete.
TEST(ScatterGatherSelect, AnUnreachableOwnerFailsTheQuery) {
    TempDir dir_a;
    TempDir dir_b;
    ReplicationManager repl_a{kNodeA};
    ReplicationManager repl_b{kNodeB};
    // The partition bodies travel peer-to-peer over the large-object store, so
    // the clusters need one — a node without it never receives what it owns.
    ReplicatedBlobStore blobs_a{dir_a.path(), repl_a};
    ReplicatedBlobStore blobs_b{dir_b.path(), repl_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43714;
    cfg_a.port_range = 1;
    Cluster cluster_a{kNodeA, cfg_a, &repl_a, &blobs_a};
    cluster_a.start();
    a.attach_cluster(cluster_a);

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43715;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43714"};
    Cluster cluster_b{kNodeB, cfg_b, &repl_b, &blobs_b};
    cluster_b.start();
    b.attach_cluster(cluster_b);

    ASSERT_TRUE(wait_for([&] {
        return cluster_a.peer_count() == 1 && cluster_b.peer_count() == 1;
    }));

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 8;
    options.replication_factor = 1;  // no replica to fall back on
    options.partition_only = true;
    options.members = {kNodeA, kNodeB};
    const AddDatasetResult added = add_dataset_rows(a, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    const bool b_owns_something =
        std::any_of(added.partitions.begin(), added.partitions.end(),
                    [](const auto& partition) {
                        const auto& owners = partition->owner_nodes();
                        return std::find(owners.begin(), owners.end(), kNodeB) !=
                               owners.end();
                    });
    ASSERT_TRUE(b_owns_something) << "nothing was placed on the node we take away";

    ASSERT_TRUE(converge(a, b, [&] {
        return b.dataset_partition_count() == added.partitions.size();
    }));

    // Take B away. The catalog still says B owns those partitions, and there is
    // no second copy — so the query must FAIL rather than answer from A's share.
    b.stop();
    cluster_b.stop();
    ASSERT_TRUE(wait_for([&] {
        a.tick();
        return cluster_a.peer_count() == 0;
    }, std::chrono::seconds(30)));

    QueryService service{a};
    const auto outcome = service.run(kGroupedSql, "");
    EXPECT_FALSE(outcome.ok())
        << "answering from a subset of the shards is the outcome that must not ship";
    EXPECT_NE(outcome.error.find("no reachable owner"), std::string::npos)
        << outcome.error;

    a.stop();
    cluster_a.stop();
}
