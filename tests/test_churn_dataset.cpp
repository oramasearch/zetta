#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/checks/entity_resolution_check.hpp"
#include "zetta/churn_dataset.hpp"
#include "zetta/database.hpp"
#include "zetta/gate.hpp"
#include "zetta/import.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/table.hpp"
#include "zetta/validation.hpp"

namespace fs = std::filesystem;
using zetta::ChurnAccount;
using zetta::ChurnDataFormat;
using zetta::ChurnDataset;
using zetta::ChurnEvent;
using zetta::CheckStatus;
using zetta::CorrectnessGate;
using zetta::Database;
using zetta::entity_resolution_attribute;
using zetta::EntityResolutionCheck;
using zetta::import_records;
using zetta::load_churn_dataset;
using zetta::ValidationReport;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::test::TempDir;

namespace {

// The committed fixture lives at <fixtures>/churn (ZETTA_TEST_FIXTURES_DIR is a
// compile-define pointing at tests/fixtures).
fs::path churn_dir() { return fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn"; }

fs::path write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return path;
}

// A minimal-but-valid manifest, for the malformed-record boundary tests.
const char* kValidManifest = R"JSON({
  "name": "t",
  "entity_field": "account_id",
  "time_field": "event_ts",
  "label_field": "churned",
  "as_of_date": "2026-01-01",
  "churn_horizon_days": 30,
  "label_definition": "test"
})JSON";

const ChurnAccount* find_account(const ChurnDataset& dataset, const std::string& id) {
    for (const ChurnAccount& account : dataset.accounts) {
        if (account.account_id == id) return &account;
    }
    return nullptr;
}

}  // namespace

// ---- the fixture loads + schema-validates -----------------------------------

TEST(ChurnDataset, LoadsTheCommittedJsonlFixture) {
    const auto result = load_churn_dataset(churn_dir());
    ASSERT_TRUE(result.ok()) << result.error;

    const ChurnDataset& dataset = result.dataset;
    EXPECT_EQ(dataset.name, "synthetic-churn-uc1");
    EXPECT_EQ(dataset.as_of_date, "2026-03-01");
    EXPECT_EQ(dataset.churn_horizon_days, 90);
    EXPECT_EQ(dataset.entity_field, "account_id");
    EXPECT_EQ(dataset.time_field, "event_ts");
    EXPECT_EQ(dataset.label_field, "churned");
    EXPECT_EQ(dataset.accounts.size(), 10u);
    EXPECT_EQ(dataset.events.size(), 27u);
}

TEST(ChurnDataset, CarriesABinaryChurnLabel) {
    const auto result = load_churn_dataset(churn_dir());
    ASSERT_TRUE(result.ok()) << result.error;

    std::size_t churned = 0;
    for (const ChurnAccount& account : result.dataset.accounts) {
        if (account.churned) ++churned;
    }
    EXPECT_EQ(churned, 3u);

    // A churned account names a churn_date; an active one does not.
    const ChurnAccount* office = find_account(result.dataset, "acct_fedex_office");
    ASSERT_NE(office, nullptr);
    EXPECT_TRUE(office->churned);
    EXPECT_FALSE(office->churn_date.empty());

    const ChurnAccount* freight = find_account(result.dataset, "acct_fedex_freight");
    ASSERT_NE(freight, nullptr);
    EXPECT_FALSE(freight->churned);
    EXPECT_TRUE(freight->churn_date.empty());
}

// ---- the FedEx entity-resolution case (separate accounts) -------------------

TEST(ChurnDataset, FedExSiblingsAreSeparateAccounts) {
    const auto result = load_churn_dataset(churn_dir());
    ASSERT_TRUE(result.ok()) << result.error;

    std::unordered_set<std::string> sibling_accounts;
    std::unordered_set<std::string> sibling_entities;
    for (const ChurnAccount& account : result.dataset.accounts) {
        if (account.brand == "FedEx") {
            sibling_accounts.insert(account.account_id);
            sibling_entities.insert(account.entity);
        }
    }
    // FedEx Freight + 5 siblings, each its own account and its own entity.
    EXPECT_EQ(sibling_accounts.size(), 6u);
    EXPECT_EQ(sibling_entities.size(), 6u);
    EXPECT_TRUE(sibling_accounts.contains("acct_fedex_freight"));
    EXPECT_TRUE(sibling_entities.contains("fedex_freight"));
}

// ---- point-in-time: events span the as-of cutoff (the leakage trap) ---------

TEST(ChurnDataset, EventsStraddleTheAsOfDate) {
    const auto result = load_churn_dataset(churn_dir());
    ASSERT_TRUE(result.ok()) << result.error;

    const std::string& as_of = result.dataset.as_of_date;  // "YYYY-MM-DD"
    std::size_t before = 0, after = 0;
    for (const ChurnEvent& event : result.dataset.events) {
        const std::string date = event.event_ts.substr(0, 10);  // ISO date prefix
        if (date < as_of) ++before;
        if (date > as_of) ++after;
    }
    // Most events are point-in-time (<= t0); a few are after t0 and must be
    // dropped by a correct featurizer.
    EXPECT_GT(before, 0u);
    EXPECT_GT(after, 0u);
}

// ---- the JSONL and CSV forms are the same dataset ---------------------------

TEST(ChurnDataset, JsonlAndCsvLoadIdentically) {
    const auto from_jsonl = load_churn_dataset(churn_dir(), ChurnDataFormat::Jsonl);
    const auto from_csv = load_churn_dataset(churn_dir(), ChurnDataFormat::Csv);
    ASSERT_TRUE(from_jsonl.ok()) << from_jsonl.error;
    ASSERT_TRUE(from_csv.ok()) << from_csv.error;

    const ChurnDataset& a = from_jsonl.dataset;
    const ChurnDataset& b = from_csv.dataset;
    ASSERT_EQ(a.accounts.size(), b.accounts.size());
    ASSERT_EQ(a.events.size(), b.events.size());

    for (std::size_t i = 0; i < a.accounts.size(); ++i) {
        EXPECT_EQ(a.accounts[i].account_id, b.accounts[i].account_id);
        EXPECT_EQ(a.accounts[i].entity, b.accounts[i].entity);
        EXPECT_EQ(a.accounts[i].brand, b.accounts[i].brand);
        EXPECT_EQ(a.accounts[i].plan_tier, b.accounts[i].plan_tier);
        EXPECT_DOUBLE_EQ(a.accounts[i].mrr, b.accounts[i].mrr);
        EXPECT_EQ(a.accounts[i].seats, b.accounts[i].seats);
        EXPECT_EQ(a.accounts[i].tenure_days, b.accounts[i].tenure_days);
        EXPECT_EQ(a.accounts[i].churned, b.accounts[i].churned);
        EXPECT_EQ(a.accounts[i].churn_date, b.accounts[i].churn_date);
    }
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        EXPECT_EQ(a.events[i].event_id, b.events[i].event_id);
        EXPECT_EQ(a.events[i].account_id, b.events[i].account_id);
        EXPECT_EQ(a.events[i].event_ts, b.events[i].event_ts);
        EXPECT_EQ(a.events[i].event_type, b.events[i].event_type);
        EXPECT_EQ(a.events[i].mention, b.events[i].mention);
        EXPECT_DOUBLE_EQ(a.events[i].value, b.events[i].value);
    }
}

// ---- loadable into a zetta table --------------------------------------------

TEST(ChurnDataset, AccountsImportIntoAZettaTable) {
    TempDir tmp;
    Database db{"churn", "d", tmp.path() / "db"};
    auto table = db.create_table("accounts");

    const auto result = import_records(*table, churn_dir() / "accounts.jsonl");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.added, 10u);
    EXPECT_EQ(table->row_count(), 10u);
}

// ---- TC-4: division-correct resolution passes the gate ----------------------

TEST(ChurnDataset, DivisionCorrectResolutionPassesTheGate) {
    const auto result = load_churn_dataset(churn_dir());
    ASSERT_TRUE(result.ok()) << result.error;

    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts",
                   entity_resolution_attribute(result.dataset));

    const CorrectnessGate gate = zetta::default_gate();
    const ValidationReport report = gate.run(graph);
    EXPECT_TRUE(report.passed());
    EXPECT_FALSE(report.block_deploy);
}

// ---- TC-4: conflating two siblings fails the gate (RULE-1) ------------------

TEST(ChurnDataset, ConflatingTwoSiblingsBlocksDeploy) {
    const auto result = load_churn_dataset(churn_dir());
    ASSERT_TRUE(result.ok()) << result.error;

    // Take the division-correct payload, then merge the first two *distinct*
    // entities onto one account — the canonical TC-4 conflation.
    nlohmann::json attribute = entity_resolution_attribute(result.dataset);
    nlohmann::json& records = attribute["entity_resolution"]["records"];
    ASSERT_FALSE(records.empty());
    const std::string first_entity = records[0]["entity"].get<std::string>();
    const std::string first_account = records[0]["resolved"].get<std::string>();
    bool conflated = false;
    for (auto& record : records) {
        if (record["entity"].get<std::string>() != first_entity) {
            record["resolved"] = first_account;  // a sibling onto Freight's account
            conflated = true;
            break;
        }
    }
    ASSERT_TRUE(conflated);

    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "resolve_accounts", attribute);

    const CorrectnessGate gate = zetta::default_gate();
    const ValidationReport report = gate.run(graph);
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.block_deploy);  // RULE-1: conflation blocks deploy

    const zetta::CheckResult check = EntityResolutionCheck{}.evaluate(graph);
    EXPECT_EQ(check.status, CheckStatus::Fail);
    EXPECT_NE(check.reason.find(first_account), std::string::npos);
}

// ---- the schema boundary: malformed records fail fast -----------------------

TEST(ChurnDataset, MalformedAccountFailsAtTheBoundary) {
    TempDir tmp;
    const fs::path dir = tmp.path() / "bad";
    fs::create_directories(dir);
    write_file(dir / "dataset.json", kValidManifest);
    write_file(dir / "events.jsonl", "");
    // `entity` is required; this account omits it.
    write_file(dir / "accounts.jsonl",
               R"({"account_id":"a","name":"A","brand":"","plan_tier":"enterprise",)"
               R"("industry":"x","region":"us","mrr":1,"seats":1,"tenure_days":1,)"
               R"("renewal_date":"2026-01-01","churned":false,"churn_date":""})"
               "\n");

    const auto result = load_churn_dataset(dir);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("accounts.jsonl"), std::string::npos);
}

TEST(ChurnDataset, MalformedCsvCellFailsAtTheBoundary) {
    TempDir tmp;
    const fs::path dir = tmp.path() / "badcsv";
    fs::create_directories(dir);
    write_file(dir / "dataset.json", kValidManifest);
    write_file(dir / "events.csv", "event_id,account_id,event_ts,event_type,channel,mention,value\n");
    // `churned` must coerce to a boolean; "maybe" cannot.
    write_file(dir / "accounts.csv",
               "account_id,name,brand,entity,plan_tier,industry,region,mrr,seats,"
               "tenure_days,renewal_date,churned,churn_date\n"
               "a,A,,e,enterprise,x,us,1,1,1,2026-01-01,maybe,\n");

    const auto result = load_churn_dataset(dir, ChurnDataFormat::Csv);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("boolean"), std::string::npos);
}

// ---- the Parquet form is the same dataset (native reader, #508) -------------

TEST(ChurnDataset, ParquetFormatLoadsIdentically) {
    const auto from_jsonl = load_churn_dataset(churn_dir(), ChurnDataFormat::Jsonl);
    const auto from_parquet = load_churn_dataset(churn_dir(), ChurnDataFormat::Parquet);
    ASSERT_TRUE(from_jsonl.ok()) << from_jsonl.error;
    ASSERT_TRUE(from_parquet.ok()) << from_parquet.error;

    const ChurnDataset& a = from_jsonl.dataset;
    const ChurnDataset& b = from_parquet.dataset;
    ASSERT_EQ(a.accounts.size(), b.accounts.size());
    ASSERT_EQ(a.events.size(), b.events.size());

    for (std::size_t i = 0; i < a.accounts.size(); ++i) {
        EXPECT_EQ(a.accounts[i].account_id, b.accounts[i].account_id);
        EXPECT_EQ(a.accounts[i].name, b.accounts[i].name);
        EXPECT_EQ(a.accounts[i].entity, b.accounts[i].entity);
        EXPECT_EQ(a.accounts[i].brand, b.accounts[i].brand);
        EXPECT_EQ(a.accounts[i].plan_tier, b.accounts[i].plan_tier);
        EXPECT_EQ(a.accounts[i].industry, b.accounts[i].industry);
        EXPECT_EQ(a.accounts[i].region, b.accounts[i].region);
        EXPECT_DOUBLE_EQ(a.accounts[i].mrr, b.accounts[i].mrr);
        EXPECT_EQ(a.accounts[i].seats, b.accounts[i].seats);
        EXPECT_EQ(a.accounts[i].tenure_days, b.accounts[i].tenure_days);
        EXPECT_EQ(a.accounts[i].renewal_date, b.accounts[i].renewal_date);
        EXPECT_EQ(a.accounts[i].churned, b.accounts[i].churned);
        EXPECT_EQ(a.accounts[i].churn_date, b.accounts[i].churn_date);
    }
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        EXPECT_EQ(a.events[i].event_id, b.events[i].event_id);
        EXPECT_EQ(a.events[i].account_id, b.events[i].account_id);
        EXPECT_EQ(a.events[i].event_ts, b.events[i].event_ts);
        EXPECT_EQ(a.events[i].event_type, b.events[i].event_type);
        EXPECT_EQ(a.events[i].channel, b.events[i].channel);
        EXPECT_EQ(a.events[i].mention, b.events[i].mention);
        EXPECT_DOUBLE_EQ(a.events[i].value, b.events[i].value);
    }
}
