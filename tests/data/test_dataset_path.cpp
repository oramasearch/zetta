// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
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
    // with nothing saying so. With two rows holding two values, `plan` identifies rows
    // rather than grouping them (#1203), so it is still left out — and said.
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
    EXPECT_NE(result.error.find("label::int"), std::string::npos);
}

TEST(DatasetContract, AStringEntityColumnGetsAStableReversibleKey) {
    std::vector<Json> sample;
    for (int index = 0; index < 6; ++index) {
        sample.push_back(Json{{"account_id", "acct_" + std::to_string(index)},
                              {"mrr", 100.0 + index},
                              {"churned", index % 2}});
    }
    const auto result =
        infer_dataset_contract({"account_id", "mrr", "churned"}, sample);
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_TRUE(result.contract.entity_keys);
    const auto key = result.contract.entity_keys->derived("acct_3");
    ASSERT_TRUE(key);
    EXPECT_EQ(zetta::data::entity_output(result.contract, *key), "acct_3");
    EXPECT_TRUE(std::ranges::any_of(result.notes, [](const std::string& note) {
        return note == "entity column 'account_id' (text; keyed by a derived id, "
                       "outputs carry the original value)";
    }));
}

TEST(DatasetContract, AForcedDerivedKeyCollisionFailsClearly) {
    const auto result = zetta::data::derive_entity_key_mapping(
        "customer_id", {"c00041", "c00042"},
        [](std::string_view) {
            return std::uint64_t{7};
        });
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("collision"), std::string::npos);
    EXPECT_NE(result.error.find("c00041"), std::string::npos);
    EXPECT_NE(result.error.find("c00042"), std::string::npos);
}

TEST(DatasetContract, BooleanLabelsAndFeaturesAreAcceptedAsNumeric) {
    const std::vector<Json> sample{
        {{"customer_id", "c00041"}, {"active", true}, {"churned", false}},
        {{"customer_id", "c00042"}, {"active", false}, {"churned", true}}};
    const auto result = infer_dataset_contract(
        {"customer_id", "active", "churned"}, sample);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.contract.label_column, "churned");
    EXPECT_EQ(result.contract.feature_columns,
              (std::vector<std::string>{"active"}));
}

TEST(DatasetContract, AnExplicitTextLabelIsRejectedWithACastRemedy) {
    DatasetContract overrides;
    overrides.label_column = "churned";
    const std::vector<Json> sample{
        {{"customer_id", "c00041"}, {"spend", 10}, {"churned", "no"}},
        {{"customer_id", "c00042"}, {"spend", 20}, {"churned", "yes"}}};
    const auto result = infer_dataset_contract(
        {"customer_id", "spend", "churned"}, sample, overrides);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("label column 'churned'"), std::string::npos);
    EXPECT_NE(result.error.find("churned::int"), std::string::npos);
}

TEST(DatasetContract, AnExplicitContinuousNumericLabelRemainsAccepted) {
    DatasetContract overrides;
    overrides.label_column = "demand";
    const std::vector<Json> sample{
        {{"account_id", 1}, {"price", 10.0}, {"demand", 42.5}},
        {{"account_id", 2}, {"price", 11.0}, {"demand", 38.0}}};
    const auto result = infer_dataset_contract(
        {"account_id", "price", "demand"}, sample, overrides);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.contract.label_column, "demand");
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

// --- categorical features (#1203) -------------------------------------------

// A churn table as a CSV upload lands it: numbers, two low-cardinality text columns,
// a unique text column, and text dates.
std::vector<Json> categorical_sample(std::size_t rows = 40) {
    const std::vector<std::string> plans{"basic", "pro", "enterprise"};
    const std::vector<std::string> regions{"north", "south"};
    std::vector<Json> sample;
    for (std::size_t index = 0; index < rows; ++index) {
        sample.push_back(Json{{"account_id", static_cast<std::int64_t>(index + 1)},
                              {"spend", static_cast<double>(index * 3 % 17)},
                              {"plan", plans[index % 3]},
                              {"region", index % 5 == 0 ? Json(nullptr)
                                                        : Json(regions[index % 2])},
                              {"email", "user" + std::to_string(index) + "@example.com"},
                              {"signed_up", "2026-0" + std::to_string(1 + index % 9) + "-01"},
                              {"churned", index % 4 == 0 ? 1 : 0}});
    }
    return sample;
}

const std::vector<std::string> kCategoricalColumns = {
    "account_id", "spend", "plan", "region", "email", "signed_up", "churned"};

TEST(CategoricalFeatures, LowCardinalityTextBecomesAOneHotFeature) {
    const auto inferred =
        infer_dataset_contract(kCategoricalColumns, categorical_sample(), {});
    ASSERT_TRUE(inferred.ok()) << inferred.error;
    EXPECT_EQ(inferred.contract.feature_columns, (std::vector<std::string>{"spend"}));
    ASSERT_EQ(inferred.contract.categorical_features.size(), 2u);
    const auto& plan = inferred.contract.categorical_features[0];
    EXPECT_EQ(plan.column, "plan");
    EXPECT_EQ(plan.levels, (std::vector<std::string>{"basic", "enterprise", "pro"}));
    EXPECT_EQ(plan.feature_names(),
              (std::vector<std::string>{"plan=basic", "plan=enterprise", "plan=pro"}));
    // Nulls are skipped when fitting, not a level of their own.
    EXPECT_EQ(inferred.contract.categorical_features[1].levels,
              (std::vector<std::string>{"north", "south"}));

    // Every decision is in the notes: what was encoded, and why the rest was not.
    const auto note = [&inferred](std::string_view prefix) {
        const auto found = std::ranges::find_if(inferred.notes, [prefix](const auto& line) {
            return line.starts_with(prefix);
        });
        return found == inferred.notes.end() ? std::string{} : *found;
    };
    EXPECT_NE(note("categorical column: 'plan'").find("3 levels"), std::string::npos);
    EXPECT_NE(note("not used: 'email'").find("an identifier or free text"), std::string::npos)
        << note("not used: 'email'");
    EXPECT_NE(note("not used: 'signed_up'").find("dates or timestamps"), std::string::npos)
        << note("not used: 'signed_up'");
}

TEST(CategoricalFeatures, TextAboveTheLevelCapStaysExcluded) {
    std::vector<Json> sample;
    for (std::size_t index = 0; index < 200; ++index) {
        sample.push_back(Json{{"account_id", static_cast<std::int64_t>(index)},
                              {"spend", static_cast<double>(index % 9)},
                              {"city", "city_" + std::to_string(index % 40)},
                              {"churned", index % 2}});
    }
    const auto inferred =
        infer_dataset_contract({"account_id", "spend", "city", "churned"}, sample, {});
    ASSERT_TRUE(inferred.ok()) << inferred.error;
    EXPECT_TRUE(inferred.contract.categorical_features.empty());
    EXPECT_TRUE(std::ranges::any_of(inferred.notes, [](const std::string& line) {
        return line.starts_with("not used: 'city'") &&
               line.find("40 distinct values") != std::string::npos &&
               line.find("32-level cap") != std::string::npos;
    })) << inferred.notes.back();
}

TEST(CategoricalFeatures, CategoriesAloneAreATrainingSet) {
    std::vector<Json> sample;
    for (std::size_t index = 0; index < 12; ++index) {
        sample.push_back(Json{{"account_id", static_cast<std::int64_t>(index)},
                              {"plan", index % 2 == 0 ? "basic" : "pro"},
                              {"churned", index % 2}});
    }
    const auto inferred = infer_dataset_contract({"account_id", "plan", "churned"}, sample);
    ASSERT_TRUE(inferred.ok()) << inferred.error;
    EXPECT_TRUE(inferred.contract.feature_columns.empty());
    ASSERT_EQ(inferred.contract.categorical_features.size(), 1u);
}

TEST(CategoricalFeatures, ANamedTextFeatureIsAlsoRecordedAsACategory) {
    // A named text column stays in feature_columns (a text methodology reads it from
    // there) and is recorded as a category for a trainer that one-hot encodes it.
    DatasetContract overrides;
    overrides.feature_columns = {"spend", "plan"};
    const auto inferred =
        infer_dataset_contract(kCategoricalColumns, categorical_sample(), overrides);
    ASSERT_TRUE(inferred.ok()) << inferred.error;
    EXPECT_EQ(inferred.contract.feature_columns, (std::vector<std::string>{"spend", "plan"}));
    ASSERT_EQ(inferred.contract.categorical_features.size(), 1u);
    EXPECT_EQ(inferred.contract.categorical_features[0].column, "plan");
}

TEST(CategoricalFeatures, EncodingARowHandlesSeenUnseenMissingAndWrongTypes) {
    zetta::data::CategoricalEncoding plan;
    plan.column = "plan";
    plan.levels = {"basic", "pro"};
    const std::vector encodings{plan};

    Json seen{{"plan", "pro"}};
    EXPECT_EQ(zetta::data::encode_categorical_row(encodings, seen), "");
    EXPECT_EQ(seen.at("plan=basic"), 0.0);
    EXPECT_EQ(seen.at("plan=pro"), 1.0);

    Json unseen{{"plan", "platinum"}};
    EXPECT_EQ(zetta::data::encode_categorical_row(encodings, unseen), "");
    EXPECT_EQ(unseen.at("plan=basic"), 0.0);
    EXPECT_EQ(unseen.at("plan=pro"), 0.0);

    Json null_value{{"plan", nullptr}};
    EXPECT_EQ(zetta::data::encode_categorical_row(encodings, null_value), "");
    EXPECT_EQ(null_value.at("plan=pro"), 0.0);

    // Already encoded by the caller: left as it is.
    Json pre_encoded{{"plan=basic", 1}, {"plan=pro", 0}};
    EXPECT_EQ(zetta::data::encode_categorical_row(encodings, pre_encoded), "");
    EXPECT_EQ(pre_encoded.at("plan=basic"), 1);

    Json absent{{"spend", 3}};
    EXPECT_NE(zetta::data::encode_categorical_row(encodings, absent).find("no 'plan'"),
              std::string::npos);
    Json number{{"plan", 2}};
    EXPECT_NE(zetta::data::encode_categorical_row(encodings, number).find("must be text"),
              std::string::npos);
}

TEST(CategoricalFeatures, TheContractCarriesItsCategoriesThroughJson) {
    const auto inferred =
        infer_dataset_contract(kCategoricalColumns, categorical_sample(), {});
    ASSERT_TRUE(inferred.ok()) << inferred.error;
    const Json encoded = zetta::data::to_json(inferred.contract);
    ASSERT_TRUE(encoded.contains("categorical_features"));
    EXPECT_EQ(encoded.at("categorical_features")[0].at("features"),
              Json::array({"plan=basic", "plan=enterprise", "plan=pro"}));
    std::string error;
    const auto decoded = zetta::data::dataset_contract_from_json(encoded, &error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(*decoded, inferred.contract);

    Json duplicate = encoded;
    duplicate["categorical_features"][0]["levels"] = Json::array({"pro", "pro"});
    EXPECT_FALSE(zetta::data::dataset_contract_from_json(duplicate, &error));
    EXPECT_NE(error.find("duplicate level"), std::string::npos) << error;
}

}  // namespace
