#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"

// The one `dataset_path` grammar and the column contract (#947).
//
// These are the rules that decide what a customer's table MEANS before anything
// trains on it, so they are tested as rules rather than through a run: a wrong
// inference here produces a model that is confidently about the wrong column, and
// the run itself would still look healthy.
namespace {

using zetta::data::DatasetContract;
using zetta::data::DatasetPathKind;
using zetta::data::infer_dataset_contract;
using zetta::data::parse_dataset_path;
using Json = nlohmann::json;

// A flat, connector-shaped table: one row per account, engineered numeric columns,
// a binary label. This is what a warehouse query actually returns.
std::vector<Json> accounts_sample(std::size_t rows = 8) {
    std::vector<Json> sample;
    for (std::size_t index = 0; index < rows; ++index) {
        sample.push_back(Json{{"account_id", static_cast<std::int64_t>(index + 1)},
                              {"failed_payments", static_cast<std::int64_t>(index % 4)},
                              {"logins_30d", static_cast<double>(40 - index)},
                              {"nps", static_cast<double>(9 - (index % 5))},
                              {"churned", index % 3 == 0 ? 1 : 0}});
    }
    return sample;
}

const std::vector<std::string> kAccountColumns = {"account_id", "failed_payments",
                                                  "logins_30d", "nps", "churned"};

// --- the grammar -------------------------------------------------------------

TEST(DatasetPath, DatabaseDotTableParsesAsATable) {
    const auto parsed = parse_dataset_path("churn_demo.accounts");
    EXPECT_EQ(parsed.kind, DatasetPathKind::Table);
    EXPECT_EQ(parsed.database, "churn_demo");
    EXPECT_EQ(parsed.table, "accounts");
}

TEST(DatasetPath, ABareNameTakesTheDefaultDatabase) {
    const auto parsed = parse_dataset_path("accounts", "datasets");
    EXPECT_EQ(parsed.kind, DatasetPathKind::Table);
    EXPECT_EQ(parsed.database, "datasets");
    EXPECT_EQ(parsed.table, "accounts");
}

// A separator means a path, even when the name also contains a dot — this is the
// case that made the grammar ambiguous in the first place.
TEST(DatasetPath, ASeparatorAlwaysMeansADirectory) {
    for (const char* path : {"/var/lib/churn", "./data/churn.v2", "data/churn",
                             "~/fixtures/churn"}) {
        const auto parsed = parse_dataset_path(path);
        EXPECT_EQ(parsed.kind, DatasetPathKind::Directory) << path;
        EXPECT_EQ(parsed.directory.string(), path) << path;
    }
}

// A qualified name keeps only the LAST dot as the separator, so a database whose
// name contains a dot still resolves.
TEST(DatasetPath, TheLastDotSeparatesDatabaseFromTable) {
    const auto parsed = parse_dataset_path("prod.eu.accounts");
    EXPECT_EQ(parsed.kind, DatasetPathKind::Table);
    EXPECT_EQ(parsed.database, "prod.eu");
    EXPECT_EQ(parsed.table, "accounts");
}

// --- inference ---------------------------------------------------------------

TEST(DatasetContract, InfersEntityLabelAndFeaturesFromAFlatTable) {
    const auto result = infer_dataset_contract(kAccountColumns, accounts_sample());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.contract.entity_column, "account_id");
    EXPECT_EQ(result.contract.label_column, "churned");
    EXPECT_EQ(result.contract.feature_columns,
              (std::vector<std::string>{"failed_payments", "logins_30d", "nps"}));
    // Never inferred: a cutoff decides the leakage boundary.
    EXPECT_TRUE(result.contract.time_column.empty());
    EXPECT_FALSE(result.notes.empty());
}

TEST(DatasetContract, AColumnItCannotUseIsSaidOutLoud) {
    // lake's UX review 2026-09-27: a categorical `plan` column vanished from the features
    // with nothing saying so (#957 is categorical support itself).
    const std::vector<std::string> columns{"account_id", "plan", "spend", "churned"};
    const std::vector<Json> sample{
        {{"account_id", 1}, {"plan", "pro"}, {"spend", 80.0}, {"churned", 0}},
        {{"account_id", 2}, {"plan", "basic"}, {"spend", 20.0}, {"churned", 1}}};
    const auto inferred = zetta::data::infer_dataset_contract(columns, sample, {});
    ASSERT_TRUE(inferred.ok()) << inferred.error;
    EXPECT_EQ(inferred.contract.feature_columns, (std::vector<std::string>{"spend"}));
    EXPECT_TRUE(std::ranges::any_of(inferred.notes, [](const std::string& note) {
        return note.starts_with("not used: 'plan'");
    }));
}

// The label and entity are never carried into X — that would leak the answer.
TEST(DatasetContract, TheLabelAndEntityAreNeverFeatures) {
    const auto result = infer_dataset_contract(kAccountColumns, accounts_sample());
    ASSERT_TRUE(result.ok()) << result.error;
    for (const std::string& feature : result.contract.feature_columns) {
        EXPECT_NE(feature, result.contract.label_column);
        EXPECT_NE(feature, result.contract.entity_column);
    }
}

// Binary-ness outranks the name: a column called `label` holding free text is not a
// training target, and the failure must name what was looked for.
TEST(DatasetContract, ANonBinaryLabelColumnIsRejectedByName) {
    std::vector<Json> sample;
    for (int index = 0; index < 6; ++index) {
        sample.push_back(Json{{"account_id", index},
                              {"mrr", 100.0 + index},
                              {"label", "tier " + std::to_string(index)}});
    }
    const auto result =
        infer_dataset_contract({"account_id", "mrr", "label"}, sample);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("label"), std::string::npos);
    EXPECT_NE(result.error.find("not binary"), std::string::npos);
}

// A string account id is the common warehouse shape and the one the ML path cannot
// key on. It must fail HERE, naming the remedy — not deep inside a training job.
TEST(DatasetContract, AStringEntityColumnFailsWithTheRemedy) {
    std::vector<Json> sample;
    for (int index = 0; index < 6; ++index) {
        sample.push_back(Json{{"account_id", "acct_" + std::to_string(index)},
                              {"mrr", 100.0 + index},
                              {"churned", index % 2}});
    }
    const auto result =
        infer_dataset_contract({"account_id", "mrr", "churned"}, sample);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("entity_column"), std::string::npos);
    EXPECT_NE(result.error.find("64-bit integer"), std::string::npos);
}

// Warehouses hand NUMBER columns back as doubles; a whole-valued double is still an
// integer key, and refusing it would fail on the most common shape there is.
TEST(DatasetContract, AWholeValuedDoubleIsAValidEntityKey) {
    std::vector<Json> sample;
    for (int index = 0; index < 6; ++index) {
        sample.push_back(Json{{"account_id", static_cast<double>(index + 1)},
                              {"mrr", 100.5 + index},
                              {"churned", index % 2}});
    }
    const auto result =
        infer_dataset_contract({"account_id", "mrr", "churned"}, sample);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.contract.entity_column, "account_id");
}

TEST(DatasetContract, ALabelWithNoFeaturesIsNotATrainingSet) {
    std::vector<Json> sample;
    for (int index = 0; index < 6; ++index) {
        sample.push_back(Json{{"account_id", index}, {"churned", index % 2}});
    }
    const auto result = infer_dataset_contract({"account_id", "churned"}, sample);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("not a training set"), std::string::npos);
}

// An explicitly named column that is not there is a typo, not an intent.
TEST(DatasetContract, AnOverrideNamingAMissingColumnFails) {
    DatasetContract overrides;
    overrides.label_column = "will_churn";
    const auto result = infer_dataset_contract(kAccountColumns, accounts_sample(), overrides);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("will_churn"), std::string::npos);
    // The reason lists what the table DOES have, so the caller can fix it in one go.
    EXPECT_NE(result.error.find("account_id"), std::string::npos);
}

TEST(DatasetContract, OverridesWinOverInference) {
    DatasetContract overrides;
    overrides.label_column = "churned";
    overrides.entity_column = "account_id";
    overrides.feature_columns = {"nps"};
    const auto result = infer_dataset_contract(kAccountColumns, accounts_sample(), overrides);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.contract.feature_columns, (std::vector<std::string>{"nps"}));
}

// Naming the label as a feature is the leak the contract exists to prevent.
TEST(DatasetContract, TheLabelCannotBeNamedAsAFeature) {
    DatasetContract overrides;
    overrides.feature_columns = {"nps", "churned"};
    const auto result = infer_dataset_contract(kAccountColumns, accounts_sample(), overrides);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("cannot be a feature"), std::string::npos);
}

// A warehouse round-trips CHURNED and churned interchangeably; the contract must
// resolve to the spelling the table actually carries.
TEST(DatasetContract, ColumnMatchingIsCaseInsensitiveButKeepsTheRealSpelling) {
    std::vector<Json> sample;
    for (int index = 0; index < 6; ++index) {
        sample.push_back(Json{{"ACCOUNT_ID", index},
                              {"MRR", 100.0 + index},
                              {"CHURNED", index % 2}});
    }
    DatasetContract overrides;
    overrides.label_column = "churned";
    const auto result =
        infer_dataset_contract({"ACCOUNT_ID", "MRR", "CHURNED"}, sample, overrides);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.contract.label_column, "CHURNED");
    EXPECT_EQ(result.contract.entity_column, "ACCOUNT_ID");
}

// A time column is honoured when given, and reported — it is the leakage boundary.
TEST(DatasetContract, ATimeColumnIsUsedOnlyWhenNamed) {
    std::vector<Json> sample;
    for (int index = 0; index < 6; ++index) {
        sample.push_back(Json{{"account_id", index},
                              {"observed_day", 1000.0 + index},
                              {"mrr", 100.0 + index},
                              {"churned", index % 2}});
    }
    const std::vector<std::string> columns = {"account_id", "observed_day", "mrr", "churned"};

    // Not named: `observed_day` is just another numeric feature.
    const auto inferred = infer_dataset_contract(columns, sample);
    ASSERT_TRUE(inferred.ok()) << inferred.error;
    EXPECT_TRUE(inferred.contract.time_column.empty());
    EXPECT_NE(std::find(inferred.contract.feature_columns.begin(),
                        inferred.contract.feature_columns.end(), "observed_day"),
              inferred.contract.feature_columns.end());

    // Named: it becomes the cutoff and leaves the feature set.
    DatasetContract overrides;
    overrides.time_column = "observed_day";
    overrides.as_of = 1003.0;
    const auto cut = infer_dataset_contract(columns, sample, overrides);
    ASSERT_TRUE(cut.ok()) << cut.error;
    EXPECT_EQ(cut.contract.time_column, "observed_day");
    EXPECT_EQ(std::find(cut.contract.feature_columns.begin(),
                        cut.contract.feature_columns.end(), "observed_day"),
              cut.contract.feature_columns.end());
}

// --- the JSON boundary --------------------------------------------------------

TEST(DatasetContract, JsonRoundTrips) {
    DatasetContract contract;
    contract.entity_column = "account_id";
    contract.label_column = "churned";
    contract.time_column = "observed_day";
    contract.as_of = 1003.0;
    contract.feature_columns = {"mrr", "nps"};

    std::string error;
    const auto parsed = zetta::data::dataset_contract_from_json(to_json(contract), &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(*parsed, contract);
}

TEST(DatasetContract, IsoCutoffNormalizesToEpochSeconds) {
    std::string error;
    const auto parsed = zetta::data::dataset_contract_from_json(
        nlohmann::json{{"time_column", "observed_at"},
                       {"as_of", "1970-01-01T00:00:03.500Z"}},
        &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_DOUBLE_EQ(parsed->as_of, 3.5);
}

// A cutoff column with no cutoff decides nothing, and inferring one would decide the
// leakage boundary silently.
TEST(DatasetContract, ATimeColumnWithoutAnAsOfIsRejected) {
    std::string error;
    const auto parsed = zetta::data::dataset_contract_from_json(
        nlohmann::json{{"time_column", "observed_day"}}, &error);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_NE(error.find("as_of"), std::string::npos);
}

TEST(DatasetContract, MalformedFieldsAreRejectedAtTheBoundary) {
    std::string error;
    EXPECT_FALSE(zetta::data::dataset_contract_from_json(nlohmann::json::array(), &error));
    EXPECT_FALSE(zetta::data::dataset_contract_from_json(
        nlohmann::json{{"label_column", 7}}, &error));
    EXPECT_FALSE(zetta::data::dataset_contract_from_json(
        nlohmann::json{{"feature_columns", "mrr"}}, &error));
    EXPECT_FALSE(zetta::data::dataset_contract_from_json(
        nlohmann::json{{"feature_columns", nlohmann::json::array({""})}}, &error));
}

}  // namespace
