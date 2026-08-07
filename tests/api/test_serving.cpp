// Governed HTTP serving of models and indexes (#648, docs/api/serving.md):
// the layer-1 journeys — expose -> serve -> un-expose kills the route — with
// allow AND deny coverage, read-back through get_model/get_index and the
// system.* views (RULE-7), the predict schema boundary, and the serve:* trace
// spans. The protocol-layer (loopback HTTP) acceptance lives in
// tests/api/test_rest_server.cpp.
#include <gtest/gtest.h>

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
#include "zetta/data/table.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_step_executor.hpp"  // read_index_partitions
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/model.hpp"
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

    // Allow: a key holding exactly data:search serves, and the served hits
    // equal the `search` capability's over the same target — one path.
    const std::string serving_key = make_key(
        "customer-search", {{CapabilityDomain::Data, "search"}});
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
