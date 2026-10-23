// Governed HTTP serving of models and indexes (#648, docs/api/serving.md):
// the layer-1 journeys — expose -> serve -> un-expose kills the route — with
// allow AND deny coverage, read-back through get_model/get_index and the
// system.* views (RULE-7), the predict schema boundary, and the serve:* trace
// spans. The protocol-layer (loopback HTTP) acceptance lives in
// tests/api/test_rest_server.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "index/term_frequency_backend.hpp"
#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_step_executor.hpp"  // read_index_partitions
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/score_table.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;
using zetta::api::ServeRequest;
using zetta::governance::CapabilityDomain;

struct ServingFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::observability::TraceContext trace =
        zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input,
                   const std::string& acting_key = {}) {
        ApiContext context{node, nullptr, acting_key, trace,
                           &node.span_recorder()};
        return registry.call(name, input, context);
    }

    // A named key holding exactly the given rights (deny-by-default: nothing
    // else) — the "endpoint access is key-scoped" acceptance surface.
    std::string make_key(
        const std::string& principal_name,
        const std::vector<std::pair<CapabilityDomain, std::string>>& rights) {
        auto principal = node.create_principal(
            principal_name, zetta::governance::PrincipalKind::Service);
        EXPECT_TRUE(principal);
        zetta::governance::CapabilitySet capabilities;
        for (const auto& [domain, action] : rights) {
            capabilities.grant(domain, action);
        }
        auto key = principal->create_key(
            zetta::governance::unrestricted_scope(), capabilities);
        EXPECT_TRUE(key);
        return key->id();
    }

    ApiResult serve_predict(const std::string& target, nlohmann::json input,
                            const std::string& acting_key = {}) {
        ServeRequest request{target, std::move(input), acting_key, trace,
                             &node.span_recorder()};
        return zetta::api::serve_predict(node, request);
    }

    ApiResult serve_search(const std::string& target, nlohmann::json input,
                           const std::string& acting_key = {}) {
        ServeRequest request{target, std::move(input), acting_key, trace,
                             &node.span_recorder()};
        return zetta::api::serve_search(node, request);
    }
};

// A tiny separable trained GBDT (feature f0 carries the signal — the
// test_gbdt_score.cpp synthetic, shrunk).
zetta::ml::GbdtModel tiny_trained_model() {
    std::vector<std::vector<double>> columns(3);
    std::vector<double> labels;
    for (std::size_t i = 0; i < 40; ++i) {
        const bool positive = i >= 20;
        columns[0].push_back(positive ? 0.8 : 0.1);
        columns[1].push_back(static_cast<double>(i % 7) / 7.0);
        columns[2].push_back(static_cast<double>((i * 3) % 5) / 5.0);
        labels.push_back(positive ? 1.0 : 0.0);
    }
    zetta::ml::GbdtParams params;
    params.num_trees = 10;
    params.max_depth = 3;
    const auto trained =
        zetta::ml::train_gbdt(columns, {"f0", "f1", "f2"}, labels, params);
    EXPECT_TRUE(trained.ok()) << trained.status.message;
    return trained.model;
}

// The model acceptance: expose a trained model, predict with a key, un-expose
// kills the route — with the deny half (a key without data:predict is
// refused + audited) and read-back through get_model and system.models.
TEST_F(ServingFixture, ModelExposeServePredictDenyAndUnexpose) {
    const zetta::ml::GbdtModel gbdt = tiny_trained_model();
    zetta::ml::ModelValue value;
    value.kind = "classifier";
    value.name = "churn-v1";
    auto model = node.create_model(std::move(value));
    ASSERT_TRUE(model);
    ASSERT_TRUE(model->set_weights(gbdt.to_json().dump()));

    const nlohmann::json body{
        {"rows",
         {{{"f0", 0.9}, {"f1", 0.1}, {"f2", 0.2}},    // high risk
          {{"f0", 0.05}, {"f1", 0.1}, {"f2", 0.2}}}}};  // low risk

    // Cluster-internal by default: the route refuses before exposure.
    auto before = serve_predict("churn-v1", body);
    EXPECT_FALSE(before.ok);
    EXPECT_EQ(before.error.code, "not_found");
    EXPECT_NE(before.error.message.find("not exposed"), std::string::npos);

    // Exposure is an orchestration:<action> capability — a key without
    // orchestration:expose_model cannot expose (deny journey, control plane).
    const std::string reader = make_key(
        "reader", {{CapabilityDomain::Api, "query"}});
    auto denied_expose =
        call("expose_model", {{"model", "churn-v1"}}, reader);
    EXPECT_FALSE(denied_expose.ok);
    EXPECT_EQ(denied_expose.error.code, "unauthorized");

    // Expose (unauthenticated local — the audited loopback session), then
    // read the state back through the typed read and the system view.
    auto exposed = call("expose_model", {{"model", "churn-v1"}});
    ASSERT_TRUE(exposed.ok) << exposed.error.message;
    EXPECT_EQ(exposed.value.at("model_id"), model->id());
    EXPECT_TRUE(exposed.value.at("exposed").get<bool>());
    const std::string endpoint =
        exposed.value.at("endpoint").get<std::string>();
    EXPECT_EQ(endpoint, "/v1/models/" + model->id() + "/predict");

    auto detail = call("get_model", {{"model_id", model->id()}});
    ASSERT_TRUE(detail.ok);
    EXPECT_TRUE(detail.value.at("exposed").get<bool>());

    auto view = call("sql",
                     {{"statement", "SELECT * FROM system.models;"}});
    ASSERT_TRUE(view.ok) << view.error.message;
    ASSERT_EQ(view.value.at("rows").size(), 1u);
    EXPECT_EQ(view.value.at("rows")[0].at("exposed"), true);
    EXPECT_EQ(view.value.at("rows")[0].at("endpoint"), endpoint);

    // Allow: a key holding exactly data:predict serves; the ranking equals
    // the pure scorer (the batch-scoring path, #392) and the request records
    // a serve:predict span with the trace ids on the result.
    const std::string serving_key = make_key(
        "customer-scorer", {{CapabilityDomain::Data, "predict"}});
    auto served = serve_predict("churn-v1", body, serving_key);
    ASSERT_TRUE(served.ok) << served.error.message;
    EXPECT_EQ(served.value.at("model_id"), model->id());
    EXPECT_EQ(served.value.at("rows"), 2);
    ASSERT_EQ(served.value.at("ranked").size(), 2u);
    EXPECT_EQ(served.value.at("ranked")[0].at("entity"), 0);  // high risk first
    const auto oracle = zetta::ml::score_gbdt(
        gbdt, {{0.9, 0.05}, {0.1, 0.1}, {0.2, 0.2}}, {"f0", "f1", "f2"},
        {0, 1});
    ASSERT_TRUE(oracle.ok());
    EXPECT_DOUBLE_EQ(served.value.at("ranked")[0].at("risk").get<double>(),
                     oracle.ranked[0].risk);
    EXPECT_DOUBLE_EQ(served.value.at("ranked")[1].at("risk").get<double>(),
                     oracle.ranked[1].risk);

    const auto entity_keys = zetta::data::derive_entity_key_mapping(
        "customer_id", {"c00041", "c00042"});
    ASSERT_TRUE(entity_keys.ok()) << entity_keys.error;
    model->set_feature_schema(
        {{"entity_key_mapping", entity_keys.mapping->to_json()}});
    const nlohmann::json text_body{
        {"rows",
         {{{"f0", true}, {"f1", 0.1}, {"f2", 0.2}},
          {{"f0", false}, {"f1", 0.1}, {"f2", 0.2}}}},
        {"entities", {"c00041", "c00042"}}};
    const ApiResult text_served =
        serve_predict("churn-v1", text_body, serving_key);
    ASSERT_TRUE(text_served.ok) << text_served.error.message;
    ASSERT_EQ(text_served.value.at("ranked").size(), 2u);
    std::vector<std::string> text_entities;
    for (const auto& score : text_served.value.at("ranked")) {
        ASSERT_TRUE(score.at("entity").is_string()) << score.dump();
        text_entities.push_back(score.at("entity").get<std::string>());
    }
    std::sort(text_entities.begin(), text_entities.end());
    EXPECT_EQ(text_entities,
              (std::vector<std::string>{"c00041", "c00042"}));
    EXPECT_EQ(served.trace_id.size(), 32u);
    bool saw_serve_span = false;
    for (const auto& span : node.span_recorder().recent(20)) {
        if (span.name == "serve:predict" && span.ok) saw_serve_span = true;
    }
    EXPECT_TRUE(saw_serve_span);

    // The display-name spelling and the id spelling serve the same model.
    auto by_id = serve_predict(model->id(), body, serving_key);
    ASSERT_TRUE(by_id.ok) << by_id.error.message;
    EXPECT_EQ(by_id.value.at("ranked"), served.value.at("ranked"));

    // Deny: a key WITHOUT data:predict is refused and the refusal audited.
    const auto audit_before = node.audit_event_count();
    auto denied = serve_predict("churn-v1", body, reader);
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    ASSERT_GT(node.audit_event_count(), audit_before);
    const auto events = node.audit_events();
    EXPECT_EQ(events.back()->value().target, "serve:predict:churn-v1");
    EXPECT_FALSE(events.back()->value().allowed);

    // The schema boundary rejects a malformed body with the path-qualified
    // reason; a missing model feature names the failing row.
    auto no_rows = serve_predict("churn-v1", nlohmann::json::object(),
                                 serving_key);
    EXPECT_FALSE(no_rows.ok);
    EXPECT_EQ(no_rows.error.code, "invalid_input");
    auto missing_feature = serve_predict(
        "churn-v1", {{"rows", {{{"f0", 0.9}, {"f1", 0.1}}}}}, serving_key);
    EXPECT_FALSE(missing_feature.ok);
    EXPECT_EQ(missing_feature.error.code, "invalid_input");
    EXPECT_EQ(missing_feature.error.path, "/rows/0");
    EXPECT_NE(missing_feature.error.message.find("f2"), std::string::npos);
    auto ragged_entities = serve_predict(
        "churn-v1", {{"rows", body.at("rows")}, {"entities", {7}}},
        serving_key);
    EXPECT_FALSE(ragged_entities.ok);
    EXPECT_EQ(ragged_entities.error.code, "invalid_input");

    // Un-expose is one action: the route dies, the state reads back false.
    auto unexposed = call("unexpose_model", {{"model", "churn-v1"}});
    ASSERT_TRUE(unexposed.ok) << unexposed.error.message;
    EXPECT_FALSE(unexposed.value.at("exposed").get<bool>());
    auto after = serve_predict("churn-v1", body, serving_key);
    EXPECT_FALSE(after.ok);
    EXPECT_EQ(after.error.code, "not_found");
    // The value emits `exposed` only when set, so a withdrawn model reads
    // back without the field — i.e. not exposed (legacy-shape compatible).
    auto detail_after = call("get_model", {{"model_id", model->id()}});
    ASSERT_TRUE(detail_after.ok);
    EXPECT_FALSE(detail_after.value.value("exposed", false));

    // An unknown target is not_found, never a crash.
    auto unknown = serve_predict("no-such-model", body, serving_key);
    EXPECT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error.code, "not_found");
}

// The index acceptance: expose an index, search with a key, un-expose kills
// the route — the served route is the SAME Node::search path as the `search`
// capability (identical hits), with the deny half and system.indexes
// read-back.
// #1203: a model trained with a one-hot encoded text column is served by the
// column's ORIGINAL name and text value. The stored levels encode the row; an
// unseen level encodes as all zeros rather than failing; a number where the
// model expects text, or a row without the column, is refused.
TEST_F(ServingFixture, ServedPredictEncodesTextCategoriesWithTheStoredLevels) {
    std::vector<std::vector<double>> columns(3);
    std::vector<double> labels;
    for (std::size_t i = 0; i < 60; ++i) {
        const std::size_t tier = i % 3;  // 0 gold, 1 silver, 2 neither
        columns[0].push_back(static_cast<double>(i % 7) / 7.0);
        columns[1].push_back(tier == 0 ? 1.0 : 0.0);
        columns[2].push_back(tier == 1 ? 1.0 : 0.0);
        labels.push_back(tier == 0 ? 1.0 : 0.0);
    }
    zetta::ml::GbdtParams params;
    params.num_trees = 10;
    params.max_depth = 3;
    const auto trained = zetta::ml::train_gbdt(
        columns, {"usage", "tier=gold", "tier=silver"}, labels, params);
    ASSERT_TRUE(trained.ok()) << trained.status.message;

    zetta::ml::ModelValue value;
    value.kind = "classifier";
    value.name = "tiered";
    auto model = node.create_model(std::move(value));
    ASSERT_TRUE(model);
    ASSERT_TRUE(model->set_weights(trained.model.to_json().dump()));
    zetta::data::CategoricalEncoding tier;
    tier.column = "tier";
    tier.levels = {"gold", "silver"};
    nlohmann::json schema{{"features", {"usage", "tier=gold", "tier=silver"}},
                          {"categorical_encoding", zetta::data::to_json(
                                                       std::vector{tier})}};
    schema["inputs"] = zetta::ml::model_input_contract(schema);
    model->set_feature_schema(schema);
    ASSERT_TRUE(call("expose_model", {{"model", "tiered"}}).ok);
    const std::string key =
        make_key("scorer", {{CapabilityDomain::Data, "predict"}});

    // The input contract names the original column, typed as text.
    ASSERT_EQ(schema.at("inputs").size(), 2u);
    EXPECT_EQ(schema.at("inputs")[0], (nlohmann::json{{"name", "usage"}, {"type", "number"}}));
    EXPECT_EQ(schema.at("inputs")[1].at("name"), "tier");
    EXPECT_EQ(schema.at("inputs")[1].at("type"), "string");
    EXPECT_EQ(schema.at("inputs")[1].at("levels"), (nlohmann::json{"gold", "silver"}));

    const auto served = serve_predict(
        "tiered",
        {{"rows",
          {{{"usage", 0.3}, {"tier", "gold"}},
           {{"usage", 0.3}, {"tier", "bronze"}},  // never seen in training
           {{"usage", 0.3}, {"tier", nullptr}}}}},
        key);
    ASSERT_TRUE(served.ok) << served.error.message;
    const auto oracle = zetta::ml::score_gbdt(
        trained.model, {{0.3, 0.3, 0.3}, {1.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
        {"usage", "tier=gold", "tier=silver"}, {0, 1, 2});
    ASSERT_TRUE(oracle.ok());
    ASSERT_EQ(served.value.at("ranked").size(), 3u);
    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_EQ(served.value.at("ranked")[index].at("entity"), oracle.ranked[index].entity);
        EXPECT_DOUBLE_EQ(served.value.at("ranked")[index].at("risk").get<double>(),
                         oracle.ranked[index].risk);
    }
    EXPECT_EQ(served.value.at("ranked")[0].at("entity"), 0);  // gold is the risky tier

    const auto numeric_tier =
        serve_predict("tiered", {{"rows", {{{"usage", 0.3}, {"tier", 2}}}}}, key);
    EXPECT_FALSE(numeric_tier.ok);
    EXPECT_EQ(numeric_tier.error.code, "invalid_input");
    EXPECT_EQ(numeric_tier.error.path, "/rows/0");
    const auto no_tier = serve_predict("tiered", {{"rows", {{{"usage", 0.3}}}}}, key);
    EXPECT_FALSE(no_tier.ok);
    EXPECT_NE(no_tier.error.message.find("'tier'"), std::string::npos)
        << no_tier.error.message;
}

TEST_F(ServingFixture, IndexExposeServeSearchDenyAndUnexpose) {
    node.index_registry().add(
        std::make_shared<zetta::test::TermFrequencyBackend>());
    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("articles");
    ASSERT_TRUE(table);
    table->add_row({{"title", "Postgres deep dive"},
                    {"body", "postgres postgres postgres tuning"}});
    table->add_row({{"title", "Cooking pasta"}, {"body", "boil water"}});

    zetta::index::IndexValue value;
    value.table = "articles";
    value.kind = zetta::index::IndexKind::Search;
    value.backend = "tf_reference";
    value.fields = {"title", "body"};
    const auto index = node.create_index(std::move(value));
    ASSERT_TRUE(index);
    const zetta::index::IndexSpec spec = index->spec();
    ASSERT_TRUE(node.index_registry()
                    .resolve(spec)
                    ->build(spec, zetta::index::read_index_partitions(
                                      table->store()))
                    .ok());

    const nlohmann::json query{{"query", "postgres"}};

    // Cluster-internal by default.
    auto before = serve_search("articles", query);
    EXPECT_FALSE(before.ok);
    EXPECT_EQ(before.error.code, "not_found");
    EXPECT_NE(before.error.message.find("not exposed"), std::string::npos);

    // Expose by table name — THE search resolution rule (#641) — and read
    // the state back through get_index and system.indexes.
    auto exposed = call("expose_index", {{"index", "articles"}});
    ASSERT_TRUE(exposed.ok) << exposed.error.message;
    EXPECT_EQ(exposed.value.at("index_id"), index->id());
    EXPECT_TRUE(exposed.value.at("exposed").get<bool>());
    const std::string endpoint =
        exposed.value.at("endpoint").get<std::string>();
    EXPECT_EQ(endpoint, "/v1/indexes/" + index->id() + "/search");

    auto detail = call("get_index", {{"index_id", index->id()}});
    ASSERT_TRUE(detail.ok);
    EXPECT_TRUE(detail.value.at("exposed").get<bool>());
    auto view = call("sql",
                     {{"statement", "SELECT * FROM system.indexes;"}});
    ASSERT_TRUE(view.ok) << view.error.message;
    ASSERT_EQ(view.value.at("rows").size(), 1u);
    EXPECT_EQ(view.value.at("rows")[0].at("exposed"), true);
    EXPECT_EQ(view.value.at("rows")[0].at("endpoint"), endpoint);

    const std::string search_only = make_key(
        "search-without-source-read", {{CapabilityDomain::Data, "search"}});
    EXPECT_FALSE(serve_search("articles", query, search_only).ok);

    // Allow: a key holding data:search and SQL read rights serves, and the
    // served hits equal the `search` capability's over the same target — one
    // path.
    const std::string serving_key =
        make_key("customer-search", {{CapabilityDomain::Data, "search"},
                                     {CapabilityDomain::Data, "read"}});
    auto served = serve_search("articles", query, serving_key);
    ASSERT_TRUE(served.ok) << served.error.message;
    EXPECT_EQ(served.value.at("index_id"), index->id());
    ASSERT_GE(served.value.at("hits").size(), 1u);
    auto capability_result =
        call("search", {{"target", "articles"}, {"query", "postgres"}});
    ASSERT_TRUE(capability_result.ok);
    EXPECT_EQ(served.value.at("hits"), capability_result.value.at("hits"));
    bool saw_serve_span = false;
    for (const auto& span : node.span_recorder().recent(30)) {
        if (span.name == "serve:search" && span.ok) saw_serve_span = true;
    }
    EXPECT_TRUE(saw_serve_span);

    // Deny: a key WITHOUT data:search is refused and audited.
    const std::string reader = make_key(
        "reader-2", {{CapabilityDomain::Api, "query"}});
    const auto audit_before = node.audit_event_count();
    auto denied = serve_search("articles", query, reader);
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    ASSERT_GT(node.audit_event_count(), audit_before);
    const auto events = node.audit_events();
    EXPECT_EQ(events.back()->value().target, "serve:search:articles");
    EXPECT_FALSE(events.back()->value().allowed);

    // The schema boundary: a missing query is invalid_input.
    auto no_query = serve_search("articles", nlohmann::json::object(),
                                 serving_key);
    EXPECT_FALSE(no_query.ok);
    EXPECT_EQ(no_query.error.code, "invalid_input");

    // Un-expose kills the route everywhere; unknown targets are not_found.
    auto unexposed = call("unexpose_index", {{"index", index->id()}});
    ASSERT_TRUE(unexposed.ok) << unexposed.error.message;
    EXPECT_FALSE(unexposed.value.at("exposed").get<bool>());
    auto after = serve_search("articles", query, serving_key);
    EXPECT_FALSE(after.ok);
    EXPECT_EQ(after.error.code, "not_found");
    auto unknown = serve_search("nowhere", query, serving_key);
    EXPECT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error.code, "not_found");
}

// Exposing an unknown entity fails clearly on the control plane too, and the
// expose actions are idempotent.
TEST_F(ServingFixture, ExposeActionsFailClearlyAndAreIdempotent) {
    auto missing_model = call("expose_model", {{"model", "nope"}});
    EXPECT_FALSE(missing_model.ok);
    EXPECT_EQ(missing_model.error.code, "not_found");
    auto missing_index = call("expose_index", {{"index", "nope"}});
    EXPECT_FALSE(missing_index.ok);
    EXPECT_EQ(missing_index.error.code, "not_found");

    zetta::ml::ModelValue value;
    value.kind = "classifier";
    auto model = node.create_model(std::move(value));
    ASSERT_TRUE(model);
    for (int repeat = 0; repeat < 2; ++repeat) {
        auto exposed = call("expose_model", {{"model", model->id()}});
        ASSERT_TRUE(exposed.ok) << exposed.error.message;
        EXPECT_TRUE(exposed.value.at("exposed").get<bool>());
    }
    for (int repeat = 0; repeat < 2; ++repeat) {
        auto unexposed = call("unexpose_model", {{"model", model->id()}});
        ASSERT_TRUE(unexposed.ok) << unexposed.error.message;
        EXPECT_FALSE(unexposed.value.at("exposed").get<bool>());
    }
}

}  // namespace

// --- model_health (#983, the first Monitor capability) --------------------------------------
//
// The agent is meant to watch a deployed model and say whether it can still be trusted. Until
// this it had no tool that answers that, so it could only DESCRIBE monitoring — which is #976 at
// the methodology level: a verb the agent has no vocabulary for.
//
// Computed from what a Model record ALREADY carries. A drift capability needing a stored
// reference distribution is a schema change and a migration; this needs neither, and it answers
// the question asked first: is the number I was given honest?
TEST_F(ServingFixture, ModelHealthReadsWhetherAModelCanStillBeTrusted) {
    const auto install = [this](const nlohmann::json& metrics, const std::string& name) {
        zetta::ml::ModelValue value;
        value.kind = "classifier";
        value.name = name;
        value.metrics = metrics;
        auto model = node.create_model(std::move(value));
        return model ? model->id() : std::string{};
    };

    // No holdout: every figure was measured on the rows the model learned from. An in-sample
    // 0.868 reached a client report once where the held-out figure was 0.726.
    const std::string no_holdout = install({{"in_sample_auc", 0.868}}, "no-holdout");
    ASSERT_FALSE(no_holdout.empty());
    auto result = call("model_health", {{"model_id", no_holdout}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(result.value.at("trustworthy").get<bool>());
    bool named_the_fix = false;
    for (const auto& finding : result.value.at("findings")) {
        if (finding.get<std::string>().find("holdout_fraction") != std::string::npos) {
            named_the_fix = true;
        }
    }
    EXPECT_TRUE(named_the_fix) << "a finding must say what to do, not only what is wrong";

    // A wide gap is memorization; the held-out figure is the only one describing new accounts.
    const std::string overfit =
        install({{"in_sample_auc", 0.95}, {"held_out_auc", 0.72}}, "overfit");
    result = call("model_health", {{"model_id", overfit}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_NEAR(result.value.at("overfit_gap").get<double>(), 0.23, 1e-9);
    EXPECT_FALSE(result.value.at("trustworthy").get<bool>());

    // A model that generalizes is trusted.
    const std::string healthy =
        install({{"in_sample_auc", 0.78}, {"held_out_auc", 0.75}}, "healthy");
    result = call("model_health", {{"model_id", healthy}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_TRUE(result.value.at("trustworthy").get<bool>());

    // Uncalibrated is not disqualifying — ranking is legitimate — but it must be SAID, because a
    // threshold of 0.8 on an uncalibrated score does not mean an 80% chance.
    bool warned_about_calibration = false;
    for (const auto& finding : result.value.at("findings")) {
        if (finding.get<std::string>().find("not calibrated") != std::string::npos) {
            warned_about_calibration = true;
        }
    }
    EXPECT_TRUE(warned_about_calibration);
}

// #1224: model_health applies the run report's held-out caveats — the same function — so the
// report cannot tell a reader not to quote a figure that model_health calls trustworthy.
TEST_F(ServingFixture, ModelHealthSharesTheRunReportsHeldOutCaveats) {
    const auto install = [this](const nlohmann::json& metrics, const std::string& name,
                                const std::string& kind) {
        zetta::ml::ModelValue value;
        value.kind = kind;
        value.name = name;
        value.metrics = metrics;
        auto model = node.create_model(std::move(value));
        return model ? model->id() : std::string{};
    };
    const auto finding_naming = [](const nlohmann::json& findings, const std::string& check) {
        for (const auto& finding : findings) {
            if (finding.get<std::string>().find("(" + check + ")") != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    // Fewer than 30 held-out rows: the figure describes the split, not the model. The gap is
    // small, so the old checks alone would have trusted it.
    const std::string small = install(
        {{"in_sample_auc", 0.80}, {"held_out_auc", 0.78}, {"held_out_rows", 12}}, "small",
        "classifier");
    ASSERT_FALSE(small.empty());
    auto result = call("model_health", {{"model_id", small}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(result.value.at("trustworthy").get<bool>());
    EXPECT_TRUE(finding_naming(result.value.at("findings"), "small_holdout"))
        << result.value.dump();
    EXPECT_FALSE(finding_naming(result.value.at("findings"), "perfect_auc"));

    // A held-out AUC at or above 0.999: a separable demo set or a leaked feature.
    const std::string perfect = install(
        {{"in_sample_auc", 1.0}, {"held_out_auc", 0.999}, {"held_out_rows", 500}}, "perfect",
        "classifier");
    result = call("model_health", {{"model_id", perfect}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(result.value.at("trustworthy").get<bool>());
    EXPECT_TRUE(finding_naming(result.value.at("findings"), "perfect_auc"))
        << result.value.dump();
    EXPECT_FALSE(finding_naming(result.value.at("findings"), "small_holdout"));

    // The demo dataset's case: both at once, and each is named.
    const std::string demo = install(
        {{"in_sample_auc", 1.0}, {"held_out_auc", 1.0}, {"held_out_rows", 4}}, "demo",
        "classifier");
    result = call("model_health", {{"model_id", demo}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(result.value.at("trustworthy").get<bool>());
    EXPECT_TRUE(finding_naming(result.value.at("findings"), "small_holdout"));
    EXPECT_TRUE(finding_naming(result.value.at("findings"), "perfect_auc"));

    // The boundaries the report uses: 30 rows and an AUC below 0.999 raise neither.
    const std::string boundary = install(
        {{"in_sample_auc", 0.80}, {"held_out_auc", 0.998}, {"held_out_rows", 30}}, "boundary",
        "classifier");
    result = call("model_health", {{"model_id", boundary}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_TRUE(result.value.at("trustworthy").get<bool>()) << result.value.dump();

    // As in the report, the row-count check does not apply to a `rank` model.
    const std::string ranked = install(
        {{"in_sample_auc", 0.80}, {"held_out_auc", 0.78}, {"held_out_rows", 12}}, "ranked",
        "rank");
    result = call("model_health", {{"model_id", ranked}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(finding_naming(result.value.at("findings"), "small_holdout"));
}

// --- propose_improvement (#983, the Improve verb) --------------------------------------------
//
// The property that makes Improve gradeable: a proposal MUST carry its predicted effect. Kernel
// D7-2 — the metric the customer pays for is the metric the agent learns on — so "retrain it"
// with no predicted effect is a suggestion, not a proposal, and nothing can score it or hold it
// to account afterwards.
TEST_F(ServingFixture, ProposeImprovementCarriesAPredictedEffectOrRefuses) {
    const auto install = [this](const nlohmann::json& metrics, const std::string& name) {
        zetta::ml::ModelValue value;
        value.kind = "classifier";
        value.name = name;
        value.metrics = metrics;
        auto model = node.create_model(std::move(value));
        return model ? model->id() : std::string{};
    };

    // Adding a holdout to a model that has none is the one effect that IS predictable — not a
    // guess about the model, but a statement about what its current number means.
    const std::string in_sample_only = install({{"in_sample_auc", 0.868}}, "in-sample-only");
    ASSERT_FALSE(in_sample_only.empty());
    auto result =
        call("propose_improvement", {{"model_id", in_sample_only}, {"change", "add_holdout"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_TRUE(result.value.at("predictable").get<bool>());
    const std::string effect = result.value.at("predicted_effect").get<std::string>();
    EXPECT_NE(effect.find("FALL"), std::string::npos)
        << "the honest prediction is that the number goes DOWN, and that is the point";

    // A bare retrain must be REFUSED. An agent predicting an improvement from a retrain it has no
    // evidence for is exactly the failure this capability exists to prevent.
    result = call("propose_improvement", {{"model_id", in_sample_only}, {"change", "retrain"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(result.value.at("predictable").get<bool>());
    EXPECT_FALSE(result.value.contains("predicted_effect"))
        << "refusing must mean refusing — a rationale plus an invented number is worse than "
           "silence, because the customer plans against the number";
    EXPECT_NE(result.value.at("rationale").get<std::string>().find("what would differ"),
              std::string::npos)
        << "the refusal must say what would make the effect predictable";

    // Proposing a holdout to a model that already has one changes nothing, and it says so.
    const std::string has_holdout =
        install({{"in_sample_auc", 0.78}, {"held_out_auc", 0.75}}, "has-holdout");
    result = call("propose_improvement", {{"model_id", has_holdout}, {"change", "add_holdout"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_FALSE(result.value.at("predictable").get<bool>());
}
