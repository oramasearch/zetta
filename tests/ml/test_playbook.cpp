#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/playbook.hpp"
#include "zetta/ml/tree_shap.hpp"
#include "zetta/storage/json_store.hpp"

// The UC-1 retention playbook (#397): the deterministic driver -> action -> owner
// mapping + expected-retained-value priority, the constrained-narrator schema
// boundary + its rules-engine fallback, and the end-to-end fixture acceptance — a
// playbook generated deterministically (the offline fallback path) for the churn
// fixture's top at-risk accounts.
namespace {

namespace fs = std::filesystem;

using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::FeatureTable;
using zetta::ddf::FeaturizeSpec;
using zetta::ddf::load_data_frame;
using zetta::ddf::point_in_time_featurize;
using zetta::ddf::WindowedFeature;
using zetta::ml::ConstrainedNarrator;
using zetta::ml::DriverPlay;
using zetta::ml::GbdtModel;
using zetta::ml::GbdtParams;
using zetta::ml::GbdtTrainResult;
using zetta::ml::generate_playbook;
using zetta::ml::NarrationRequest;
using zetta::ml::OutcomeLog;
using zetta::ml::outcome_log_from_playbook;
using zetta::ml::parse_playbook_narration;
using zetta::ml::Playbook;
using zetta::ml::PlaybookAccount;
using zetta::ml::PlaybookNarration;
using zetta::ml::PlaybookOwner;
using zetta::ml::PlaybookPolicy;
using zetta::ml::PlaybookUrgency;
using zetta::ml::RulesEngineNarrator;
using zetta::ml::set_drivers_from_shap;
using zetta::ml::ShapExplanation;
using zetta::ml::train_gbdt;
using zetta::ml::tree_shap;
using zetta::ml::urgency_for;
using zetta::ml::validate_playbook_narration;
using Json = nlohmann::json;

// =====================================================================
// The editable driver dictionary — JSON boundary
// =====================================================================

TEST(PlaybookPolicy, ChurnDefaultRoundTrips) {
    const PlaybookPolicy policy = PlaybookPolicy::churn_default();
    const std::optional<PlaybookPolicy> parsed =
        PlaybookPolicy::from_json(policy.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, policy);

    // The make-or-break churn driver: involuntary billing, high save rate, Billing owner.
    const DriverPlay& failed = policy.play_for("failed_payments");
    EXPECT_EQ(failed.owner, PlaybookOwner::Billing);
    EXPECT_TRUE(failed.involuntary);
    EXPECT_NEAR(failed.save_rate, 0.75, 1e-12);
    // An unmapped driver falls back to the default (~0.35).
    EXPECT_NEAR(policy.play_for("no_such_feature").save_rate, 0.35, 1e-12);
}

TEST(PlaybookPolicy, RejectsMalformedAtTheBoundary) {
    // Not an object / missing default_play / missing drivers.
    EXPECT_FALSE(PlaybookPolicy::from_json(Json::array()).has_value());
    EXPECT_FALSE(PlaybookPolicy::from_json(Json{{"drivers", Json::object()}}).has_value());

    Json good = PlaybookPolicy::churn_default().to_json();
    // A save_rate outside [0, 1] is rejected.
    Json bad_rate = good;
    bad_rate["drivers"]["failed_payments"]["save_rate"] = 1.5;
    EXPECT_FALSE(PlaybookPolicy::from_json(bad_rate).has_value());
    // An unknown owner is rejected.
    Json bad_owner = good;
    bad_owner["drivers"]["failed_payments"]["owner"] = "ceo";
    EXPECT_FALSE(PlaybookPolicy::from_json(bad_owner).has_value());
    // An empty play string is rejected.
    Json bad_play = good;
    bad_play["default_play"]["play"] = "";
    EXPECT_FALSE(PlaybookPolicy::from_json(bad_play).has_value());
}

// =====================================================================
// SHAP -> driver reason codes + urgency
// =====================================================================

TEST(PlaybookDrivers, SetFromShapRanksByMagnitude) {
    PlaybookAccount account;
    // f1 has the largest |φ| (negative), f0 next, f2 smallest.
    set_drivers_from_shap(account, {"f0", "f1", "f2"}, {0.5, -0.9, 0.1}, /*top_k=*/2);
    EXPECT_EQ(account.dominant_driver, "f1");
    EXPECT_DOUBLE_EQ(account.dominant_value, -0.9);
    ASSERT_EQ(account.drivers.size(), 2u);
    EXPECT_EQ(account.drivers[0].feature, "f1");
    EXPECT_FALSE(account.drivers[0].pushes_churn);  // negative φ
    EXPECT_EQ(account.drivers[1].feature, "f0");
    EXPECT_TRUE(account.drivers[1].pushes_churn);   // positive φ
}

TEST(PlaybookDrivers, UrgencyBands) {
    EXPECT_EQ(urgency_for(0.9), PlaybookUrgency::High);
    EXPECT_EQ(urgency_for(0.5), PlaybookUrgency::Medium);
    EXPECT_EQ(urgency_for(0.1), PlaybookUrgency::Low);
}

// =====================================================================
// Expected-retained-value priority + deterministic ranking
// =====================================================================

PlaybookAccount account(std::int64_t entity, double risk, double arr,
                        const std::string& driver) {
    PlaybookAccount a;
    a.entity = entity;
    a.account_id = "acct_" + std::to_string(entity);
    a.risk = risk;
    a.arr = arr;
    a.dominant_driver = driver;
    a.dominant_value = 1.0;
    a.drivers = {zetta::ml::DriverContribution{
        .feature = driver, .value = 1.0, .pushes_churn = true}};
    return a;
}

TEST(PlaybookPriority, RanksByExpectedRetainedValue) {
    // a: 0.8 * 10000 * 0.75 (failed_payments) = 6000
    // b: 0.9 * 10000 * 0.40 (total_events)    = 3600
    // c: 0.5 * 30000 * 0.35 (default)         = 5250
    const std::vector<PlaybookAccount> accounts = {
        account(1, 0.8, 10000, "failed_payments"),
        account(2, 0.9, 10000, "total_events"),
        account(3, 0.5, 30000, "unmapped_driver")};

    const Playbook playbook = generate_playbook(accounts);
    ASSERT_EQ(playbook.entries.size(), 3u);
    // Highest expected retained value first — NOT highest raw churn risk (b loses).
    EXPECT_EQ(playbook.entries[0].entity, 1);
    EXPECT_NEAR(playbook.entries[0].expected_retained_value, 6000.0, 1e-6);
    EXPECT_EQ(playbook.entries[1].entity, 3);
    EXPECT_NEAR(playbook.entries[1].expected_retained_value, 5250.0, 1e-6);
    EXPECT_EQ(playbook.entries[2].entity, 2);

    // The deterministic fallback maps the driver -> play/owner.
    EXPECT_EQ(playbook.entries[0].recommended_play,
              "Billing recovery / card update outreach");
    EXPECT_EQ(playbook.entries[0].owner, PlaybookOwner::Billing);
    EXPECT_EQ(playbook.entries[0].narrator, "rules_engine");
    ASSERT_EQ(playbook.entries[0].talking_points.size(), 3u);

    // top_k keeps only the highest-priority accounts.
    const Playbook capped =
        generate_playbook(accounts, PlaybookPolicy::churn_default(), 2);
    ASSERT_EQ(capped.entries.size(), 2u);
    EXPECT_EQ(capped.entries[0].entity, 1);
    EXPECT_EQ(capped.entries[1].entity, 3);
}

TEST(PlaybookPriority, RankingIsInputOrderIndependent) {
    std::vector<PlaybookAccount> accounts = {
        account(1, 0.8, 10000, "failed_payments"),
        account(2, 0.9, 10000, "total_events"),
        account(3, 0.5, 30000, "unmapped_driver")};
    const Playbook forward = generate_playbook(accounts);
    std::reverse(accounts.begin(), accounts.end());
    const Playbook reversed = generate_playbook(accounts);
    ASSERT_EQ(forward.entries.size(), reversed.entries.size());
    for (std::size_t i = 0; i < forward.entries.size(); ++i) {
        EXPECT_EQ(forward.entries[i].entity, reversed.entries[i].entity);
    }
}

// =====================================================================
// The constrained narrator — schema boundary + fallback (never invents numbers)
// =====================================================================

NarrationRequest sample_request() {
    NarrationRequest request;
    request.entity = 1;
    request.account_id = "acct_1";
    request.risk = 0.8;
    request.arr = 10000;
    request.expected_retained_value = 6000;
    request.dominant_driver = "failed_payments";
    request.candidates = {
        zetta::ml::NarrationCandidate{.feature = "failed_payments",
                                      .value = 1.2,
                                      .pushes_churn = true,
                                      .suggested_play = "Billing recovery",
                                      .suggested_owner = PlaybookOwner::Billing,
                                      .save_rate = 0.75,
                                      .involuntary = true}};
    return request;
}

Json valid_narration_json() {
    return Json{{"account_id", "acct_1"},
                {"primary_driver", "failed_payments"},
                {"recommended_play", "Billing recovery / card update outreach"},
                {"owner", "billing"},
                {"urgency", "high"},
                {"diagnosis", "Three failed payments — involuntary churn risk."},
                {"talking_points", {"Update the card on file.", "Confirm billing contact.",
                                    "Offer a short grace period."}},
                {"confidence", 0.75}};
}

TEST(PlaybookNarrator, AcceptsAValidNarration) {
    std::string error;
    EXPECT_TRUE(validate_playbook_narration(valid_narration_json(), &error)) << error;
    const std::optional<PlaybookNarration> parsed =
        parse_playbook_narration(valid_narration_json(), sample_request(), &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->owner, PlaybookOwner::Billing);
    EXPECT_EQ(parsed->urgency, PlaybookUrgency::High);
    EXPECT_EQ(parsed->talking_points.size(), 3u);
}

TEST(PlaybookNarrator, RejectsOffSchemaAndOffContract) {
    const NarrationRequest request = sample_request();

    // Wrong number of talking points (schema).
    Json two_points = valid_narration_json();
    two_points["talking_points"] = {"a", "b"};
    EXPECT_FALSE(parse_playbook_narration(two_points, request, nullptr).has_value());

    // Confidence out of range (schema).
    Json bad_conf = valid_narration_json();
    bad_conf["confidence"] = 1.7;
    EXPECT_FALSE(parse_playbook_narration(bad_conf, request, nullptr).has_value());

    // An injected extra field the narrator must not emit — e.g. trying to restate a
    // number it should never control (additionalProperties:false catches it).
    Json extra = valid_narration_json();
    extra["expected_retained_value"] = 999999;
    EXPECT_FALSE(parse_playbook_narration(extra, request, nullptr).has_value());

    // account_id mismatch (semantic — a batched narrator cross-wiring accounts).
    Json wrong_account = valid_narration_json();
    wrong_account["account_id"] = "acct_99";
    EXPECT_FALSE(parse_playbook_narration(wrong_account, request, nullptr).has_value());

    // A hallucinated driver outside the provided candidates (semantic).
    Json bad_driver = valid_narration_json();
    bad_driver["primary_driver"] = "made_up_driver";
    EXPECT_FALSE(parse_playbook_narration(bad_driver, request, nullptr).has_value());
}

TEST(PlaybookNarrator, ConstrainedUsesValidResponseButNeverChangesNumbers) {
    // A fake completion standing in for Claude: returns a schema-valid response.
    auto completion = [](const std::string&) -> std::optional<std::string> {
        return valid_narration_json().dump();
    };
    ConstrainedNarrator narrator(completion);
    const std::vector<PlaybookAccount> accounts = {
        account(1, 0.8, 10000, "failed_payments")};
    const Playbook playbook =
        generate_playbook(accounts, PlaybookPolicy::churn_default(), narrator);
    ASSERT_EQ(playbook.entries.size(), 1u);
    // The narrator's prose + play were used.
    EXPECT_EQ(playbook.entries[0].narrator, "claude");
    EXPECT_EQ(playbook.entries[0].diagnosis,
              "Three failed payments — involuntary churn risk.");
    // But the numbers stay COMPUTED — the priority is frozen at risk x arr x save_rate.
    EXPECT_NEAR(playbook.entries[0].risk, 0.8, 1e-12);
    EXPECT_NEAR(playbook.entries[0].arr, 10000.0, 1e-12);
    EXPECT_NEAR(playbook.entries[0].save_rate, 0.75, 1e-12);
    EXPECT_NEAR(playbook.entries[0].expected_retained_value, 6000.0, 1e-6);
}

TEST(PlaybookNarrator, ConstrainedFallsBackOnFailureAndOffline) {
    const std::vector<PlaybookAccount> accounts = {
        account(1, 0.8, 10000, "failed_payments")};

    // Malformed (non-JSON) response -> fall back to the rules engine.
    ConstrainedNarrator garbage([](const std::string&) -> std::optional<std::string> {
        return std::string("not json at all");
    });
    Playbook from_garbage =
        generate_playbook(accounts, PlaybookPolicy::churn_default(), garbage);
    EXPECT_EQ(from_garbage.entries[0].narrator, "rules_engine");
    EXPECT_EQ(from_garbage.entries[0].owner, PlaybookOwner::Billing);

    // Offline (no completion reachable) -> fall back, still produces an entry.
    ConstrainedNarrator offline(
        [](const std::string&) -> std::optional<std::string> { return std::nullopt; });
    Playbook from_offline =
        generate_playbook(accounts, PlaybookPolicy::churn_default(), offline);
    ASSERT_EQ(from_offline.entries.size(), 1u);
    EXPECT_EQ(from_offline.entries[0].narrator, "rules_engine");
    EXPECT_NEAR(from_offline.entries[0].expected_retained_value, 6000.0, 1e-6);
}

// =====================================================================
// End-to-end acceptance: featurize -> train -> score + explain -> playbook
// =====================================================================

zetta::JsonStore make_store(const std::vector<Json>& rows) {
    zetta::JsonStore store;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        store.add("row-" + std::to_string(index), rows[index]);
    }
    return store;
}

std::int64_t day_of_year_2026(const std::string& iso) {
    const int month = std::stoi(iso.substr(5, 2));
    const int day = std::stoi(iso.substr(8, 2));
    static const int before[] = {0,   0,   31,  59,  90,  120, 151,
                                 181, 212, 243, 273, 304, 334};
    return before[month] + day;
}
std::int64_t event_type_code(const std::string& type) {
    if (type == "login") return 1;
    if (type == "feature_use") return 2;
    if (type == "payment_failed") return 3;
    if (type == "support_ticket") return 4;
    if (type == "csm_contact") return 5;
    if (type == "comms") return 6;
    return 0;
}

struct ChurnEncoded {
    std::vector<Json> events;
    std::vector<Json> accounts;
    std::map<std::int64_t, zetta::ChurnAccount> by_key;  // entity key -> account
    std::int64_t as_of = 0;
};

ChurnEncoded encode_churn(const zetta::ChurnDataset& dataset) {
    ChurnEncoded out;
    std::map<std::string, std::int64_t> entity_index;
    for (const zetta::ChurnAccount& account : dataset.accounts) {
        const std::int64_t key = static_cast<std::int64_t>(entity_index.size());
        entity_index.emplace(account.account_id, key);
        out.by_key.emplace(key, account);
        out.accounts.push_back({{"entity", key},
                                {"mrr", account.mrr},
                                {"seats", account.seats},
                                {"tenure_days", account.tenure_days},
                                {"churned", account.churned ? 1 : 0}});
    }
    for (const zetta::ChurnEvent& event : dataset.events) {
        out.events.push_back({{"entity", entity_index.at(event.account_id)},
                              {"ts", day_of_year_2026(event.event_ts)},
                              {"value", event.value},
                              {"type", event_type_code(event.event_type)}});
    }
    out.as_of = day_of_year_2026(dataset.as_of_date);
    return out;
}

DataFrameSchema events_schema() {
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "entity", .dtype = DataType::Int64},
                      ColumnSchema{.name = "ts", .dtype = DataType::Int64},
                      ColumnSchema{.name = "value", .dtype = DataType::Float64},
                      ColumnSchema{.name = "type", .dtype = DataType::Int64}};
    return schema;
}
DataFrameSchema accounts_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "entity", .dtype = DataType::Int64},
        ColumnSchema{.name = "mrr", .dtype = DataType::Float64},
        ColumnSchema{.name = "seats", .dtype = DataType::Int64},
        ColumnSchema{.name = "tenure_days", .dtype = DataType::Int64},
        ColumnSchema{.name = "churned", .dtype = DataType::Int64}};
    return schema;
}

FeaturizeSpec churn_spec(std::int64_t as_of) {
    FeaturizeSpec spec;
    spec.entity_column = "entity";
    spec.time_column = "ts";
    spec.as_of = static_cast<double>(as_of);
    spec.event_features = {
        WindowedFeature{.name = "failed_payments",
                        .agg = zetta::ddf::AggregateOp::Count,
                        .type_column = "type",
                        .type_value = 3.0},
        WindowedFeature{.name = "support_severity",
                        .agg = zetta::ddf::AggregateOp::Sum,
                        .value_column = "value",
                        .type_column = "type",
                        .type_value = 4.0},
        WindowedFeature{.name = "total_events", .agg = zetta::ddf::AggregateOp::Count},
    };
    spec.account_features = {"mrr", "seats", "tenure_days"};
    spec.label_column = "churned";
    return spec;
}

struct ChurnPipeline {
    FeatureTable table;
    std::map<std::int64_t, zetta::ChurnAccount> by_key;
};

ChurnPipeline build_pipeline() {
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::load_churn_dataset(churn_dir);
    EXPECT_TRUE(loaded.ok()) << loaded.error;
    const ChurnEncoded numeric = encode_churn(loaded.dataset);
    CpuDevice device;
    auto events = load_data_frame(make_store(numeric.events), events_schema(), device);
    auto accounts =
        load_data_frame(make_store(numeric.accounts), accounts_schema(), device);
    EXPECT_TRUE(events.ok());
    EXPECT_TRUE(accounts.ok());
    auto result = point_in_time_featurize(events.frame, accounts.frame,
                                          churn_spec(numeric.as_of));
    EXPECT_TRUE(result.ok()) << result.status.message;
    return ChurnPipeline{std::move(result.table), numeric.by_key};
}

GbdtModel train_churn_model(const FeatureTable& table) {
    GbdtParams params;
    params.num_trees = 30;
    params.max_depth = 3;
    params.max_bins = 16;
    params.learning_rate = 0.3;
    const GbdtTrainResult trained =
        train_gbdt(table.features, table.feature_names, table.labels, params);
    EXPECT_TRUE(trained.ok()) << trained.status.message;
    return trained.model;
}

// Assemble the per-account playbook facts from the trained model: the calibrated risk
// (predict_proba) + the per-entity TreeSHAP drivers + ARR (mrr x 12).
std::vector<PlaybookAccount> build_accounts(const ChurnPipeline& pipeline,
                                            const GbdtModel& model) {
    std::vector<PlaybookAccount> accounts;
    const FeatureTable& table = pipeline.table;
    for (std::size_t row = 0; row < table.row_count(); ++row) {
        std::vector<double> features(table.feature_count());
        for (std::size_t c = 0; c < table.feature_count(); ++c) {
            features[c] = table.features[c][row];
        }
        PlaybookAccount account;
        account.entity = table.entities[row];
        const auto found = pipeline.by_key.find(account.entity);
        if (found != pipeline.by_key.end()) {
            account.account_id = found->second.account_id;
            account.account_name = found->second.name;
            account.arr = found->second.mrr * 12.0;
        }
        account.risk = model.predict_proba(features);
        const ShapExplanation shap = tree_shap(model, features);
        set_drivers_from_shap(account, table.feature_names, shap.values);
        accounts.push_back(std::move(account));
    }
    return accounts;
}

TEST(PlaybookChurn, GeneratedDeterministicallyForTopAtRisk) {
    const ChurnPipeline pipeline = build_pipeline();
    ASSERT_GT(pipeline.table.row_count(), 0u);
    const GbdtModel model = train_churn_model(pipeline.table);
    const std::vector<PlaybookAccount> accounts = build_accounts(pipeline, model);

    // The deterministic / offline fallback path (no LLM) — the CI acceptance.
    const Playbook playbook = generate_playbook(accounts);
    ASSERT_EQ(playbook.entries.size(), accounts.size());

    // Every entry is complete + deterministic.
    for (const auto& entry : playbook.entries) {
        EXPECT_FALSE(entry.recommended_play.empty()) << "entity " << entry.entity;
        EXPECT_FALSE(entry.diagnosis.empty()) << "entity " << entry.entity;
        EXPECT_EQ(entry.talking_points.size(), 3u) << "entity " << entry.entity;
        EXPECT_EQ(entry.narrator, "rules_engine");
        // The priority is exactly risk x arr x save_rate.
        EXPECT_NEAR(entry.expected_retained_value,
                    entry.risk * entry.arr * entry.save_rate, 1e-6)
            << "entity " << entry.entity;
    }

    // The ranking is sorted by expected retained value, descending.
    for (std::size_t i = 1; i < playbook.entries.size(); ++i) {
        EXPECT_GE(playbook.entries[i - 1].expected_retained_value,
                  playbook.entries[i].expected_retained_value);
    }

    // The account with the most failed payments is driven by failed_payments — so its
    // playbook entry runs the billing play under the Billing owner.
    const std::vector<double>* failures = pipeline.table.feature("failed_payments");
    ASSERT_NE(failures, nullptr);
    const std::size_t worst = static_cast<std::size_t>(
        std::max_element(failures->begin(), failures->end()) - failures->begin());
    ASSERT_GT((*failures)[worst], 0.0);
    const std::int64_t worst_entity = pipeline.table.entities[worst];

    const auto entry = std::find_if(
        playbook.entries.begin(), playbook.entries.end(),
        [&](const auto& e) { return e.entity == worst_entity; });
    ASSERT_NE(entry, playbook.entries.end());
    EXPECT_EQ(entry->dominant_driver, "failed_payments");
    EXPECT_EQ(entry->primary_driver, "failed_payments");
    EXPECT_EQ(entry->owner, PlaybookOwner::Billing);
    EXPECT_EQ(entry->recommended_play, "Billing recovery / card update outreach");

    // The outcome log is seeded with one pending row per entry (the feedback loop).
    const OutcomeLog log = outcome_log_from_playbook(playbook, "2026-06-27T00:00:00Z");
    ASSERT_EQ(log.rows.size(), playbook.entries.size());
    for (const auto& outcome : log.rows) {
        EXPECT_FALSE(outcome.renewed.has_value());  // unresolved until the horizon closes
        EXPECT_TRUE(outcome.action_taken.empty());
        EXPECT_EQ(outcome.generated_at, "2026-06-27T00:00:00Z");
    }
    // The JSON export carries the pending fields as null.
    const Json log_json = log.to_json();
    ASSERT_TRUE(log_json.is_array());
    EXPECT_TRUE(log_json[0]["renewed"].is_null());
}

}  // namespace
