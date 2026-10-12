// lake#654: a model made on its own, not only as one step of a project's
// pipeline — trained from a dataset, a target and a methodology (train_model),
// retrained (retrain_model), brought from a file (register_model), moved into a
// project (move_model_to_project), and deleted with its internal project.
// docs/ml/standalone-models.md.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ranges>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/ml/model_import.hpp"
#include "../../src/core/base64.hpp"

namespace {

using Json = nlohmann::json;
using zetta::api::ApiContext;
using zetta::api::ApiResult;

// 400 accounts whose churn follows tickets, failed payments and short tenure,
// every 17th label flipped so it is not separable.
std::string accounts_csv() {
    std::string csv = "account_id,support_tickets,tenure_months,monthly_spend,failed_payments,"
                      "churned\n";
    for (int index = 0; index < 400; ++index) {
        const int support_tickets = (index * 7) % 11;
        const int tenure_months = 1 + (index * 13) % 72;
        const double monthly_spend = 19.0 + ((index * 37) % 400) * 0.75;
        const int failed_payments = (index * 5) % 4;
        const double risk = 0.3 * support_tickets + 0.8 * failed_payments - 0.04 * tenure_months;
        int churned = risk > 1.6 ? 1 : 0;
        if (index % 17 == 0) churned = 1 - churned;
        csv += std::format("{},{},{},{:.2f},{},{}\n", 7000 + index, support_tickets,
                           tenure_months, monthly_spend, failed_payments, churned);
    }
    return csv;
}

class StandaloneModels : public ::testing::Test {
protected:
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{"standalone-models-node"};
    zetta::Node node{node_config(), &replication};
    zetta::cluster::Cluster cluster{"standalone-models-node", cluster_config(), &replication};
    zetta::api::Registry registry;
    zetta::api::BackgroundActions background;

    zetta::NodeConfig node_config() const {
        zetta::NodeConfig config;
        config.data_dir = dir.path().string();
        config.preflight = false;
        return config;
    }

    static zetta::cluster::ClusterConfig cluster_config() {
        zetta::cluster::ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = 24870;
        config.port_range = 4;
        return config;
    }

    void SetUp() override {
        ASSERT_TRUE(node.start());
        cluster.start();
        node.attach_cluster(cluster, 2);
        zetta::api::register_core_capabilities(registry);
        node.tick();
        const ApiResult uploaded = call("upload_dataset", {{"database", "crm"},
                                                           {"table_name", "accounts"},
                                                           {"format", "csv"},
                                                           {"content", accounts_csv()}});
        ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    }

    void TearDown() override {
        background.prune();
        node.stop();
        cluster.stop();
    }

    ApiResult call(const std::string& name, const Json& input) {
        ApiContext context{node, &cluster, {}, {}, &node.span_recorder(), &background};
        return registry.call(name, input, context);
    }

    // Pump the node until the run ends; its final status.
    zetta::pipeline::RunStatus finish(const std::string& run_id) {
        for (std::size_t attempt = 0; attempt < 60000; ++attempt) {
            node.tick();
            background.prune();
            const auto run = node.find_run(run_id);
            if (run && (run->status() == zetta::pipeline::RunStatus::Succeeded ||
                        run->status() == zetta::pipeline::RunStatus::Failed)) {
                return run->status();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return zetta::pipeline::RunStatus::Running;
    }

    Json models_named(const std::string& name) {
        const ApiResult listed = call("list_models", Json::object());
        EXPECT_TRUE(listed.ok) << listed.error.message;
        Json out = Json::array();
        for (const Json& model : listed.value.at("models")) {
            if (model.at("name") == name) out.push_back(model);
        }
        return out;
    }

    Json project_names() {
        const ApiResult listed = call("list_projects", Json::object());
        EXPECT_TRUE(listed.ok) << listed.error.message;
        return listed.value.at("projects");
    }

    // Train "churn" on its own and wait for it.
    std::string train_churn(const Json& extra = Json::object()) {
        Json input = {{"name", "churn"},
                      {"methodology", "M-1"},
                      {"dataset_path", "crm.accounts"},
                      {"target", "churned"},
                      {"entity_column", "account_id"},
                      {"holdout_fraction", 0.25}};
        input.update(extra);
        const ApiResult started = call("train_model", input);
        EXPECT_TRUE(started.ok) << started.error.code << ": " << started.error.message;
        if (!started.ok) return {};
        EXPECT_TRUE(started.value.at("standalone").get<bool>());
        const std::string run_id = started.value.at("run_id");
        EXPECT_EQ(finish(run_id), zetta::pipeline::RunStatus::Succeeded);
        return started.value.at("project_id");
    }
};

TEST_F(StandaloneModels, TrainOneWithoutAProjectPeopleSee) {
    const std::string project_id = train_churn();
    ASSERT_FALSE(project_id.empty());

    const Json versions = models_named("churn");
    ASSERT_EQ(versions.size(), 1u) << versions.dump();
    const Json& model = versions[0];
    EXPECT_TRUE(model.at("standalone").get<bool>());
    EXPECT_EQ(model.at("project"), project_id);
    EXPECT_EQ(model.at("pipeline_name"), "churn");
    EXPECT_TRUE(model.at("metrics").contains("held_out_auc")) << model.dump();
    EXPECT_TRUE(model.at("has_weights").get<bool>());

    // Never listed as a project, still found by its id.
    EXPECT_TRUE(project_names().empty()) << project_names().dump();
    const auto project = node.project_by_id(project_id);
    ASSERT_TRUE(project);
    EXPECT_TRUE(project->internal());
    EXPECT_TRUE(call("get_project", {{"name", project_id}}).ok);

    // It scores like a project's model.
    const ApiResult scored = call("score", {{"model", model.at("id")},
                                            {"dataset_path", "crm.accounts"},
                                            {"top_k", 5}});
    ASSERT_TRUE(scored.ok) << scored.error.message;
    EXPECT_EQ(scored.value.at("ranked").size(), 5u);
}

TEST_F(StandaloneModels, TrainRefusesWhatItCannotTrain) {
    const ApiResult no_target = call(
        "train_model", {{"name", "x"}, {"methodology", "M-1"}, {"dataset_path", "crm.accounts"}});
    EXPECT_EQ(no_target.error.code, "invalid_input");
    EXPECT_NE(no_target.error.message.find("/target"), std::string::npos);
    const ApiResult no_dataset = call("train_model", {{"name", "x"},
                                                      {"methodology", "M-1"},
                                                      {"dataset_path", "crm.missing"},
                                                      {"target", "churned"}});
    EXPECT_EQ(no_dataset.error.code, "not_found");
    const ApiResult no_methodology = call("train_model", {{"name", "x"},
                                                          {"methodology", "M-404"},
                                                          {"dataset_path", "crm.accounts"},
                                                          {"target", "churned"}});
    EXPECT_EQ(no_methodology.error.code, "not_found");
    // A refused training leaves nothing behind.
    EXPECT_TRUE(std::ranges::none_of(node.projects(), [](const auto& p) { return p->internal(); }));

    train_churn();
    const ApiResult again = call("train_model", {{"name", "churn"},
                                                 {"methodology", "M-1"},
                                                 {"dataset_path", "crm.accounts"},
                                                 {"target", "churned"}});
    EXPECT_EQ(again.error.code, "invalid_input");
    EXPECT_NE(again.error.message.find("retrain_model"), std::string::npos);
}

TEST_F(StandaloneModels, RetrainAddsAVersionAndKeepsANewSetting) {
    const std::string project_id = train_churn();
    const std::string first = models_named("churn").at(0).at("id");

    const ApiResult retrained =
        call("retrain_model", {{"model_id", first}, {"holdout_fraction", 0.3}});
    ASSERT_TRUE(retrained.ok) << retrained.error.message;
    EXPECT_EQ(retrained.value.at("project_id"), project_id);
    EXPECT_TRUE(retrained.value.at("standalone").get<bool>());
    ASSERT_EQ(finish(retrained.value.at("run_id")), zetta::pipeline::RunStatus::Succeeded);

    const Json versions = models_named("churn");
    ASSERT_EQ(versions.size(), 2u);
    EXPECT_TRUE(std::ranges::all_of(versions, [&](const Json& v) {
        return v.at("project") == project_id && v.at("pipeline_name") == "churn";
    }));
    // The new held-out share is kept for the next retrain.
    const auto project = node.project_by_id(project_id);
    const auto graph = project->find_graph("churn");
    ASSERT_TRUE(graph);
    const auto steps = graph->graph().nodes_of_kind(zetta::ir::StepKind::Model);
    ASSERT_FALSE(steps.empty());
    EXPECT_DOUBLE_EQ(
        steps.front()->attributes.at("standalone_training").at("holdout_fraction").get<double>(),
        0.3);
}

TEST_F(StandaloneModels, BringOneScoreItAddAVersionAndRefuseOtherFormats) {
    train_churn();
    const Json trained = models_named("churn").at(0);
    const auto source = node.find_model(trained.at("id"));
    ASSERT_TRUE(source);
    const std::string weights = source->read_weights().value();

    const ApiResult brought = call("register_model",
                                   {{"name", "partner churn"},
                                    {"weights", weights},
                                    {"metrics", {{"held_out_auc", 0.81}}},
                                    {"source", "drive:models/partner-churn.json"}});
    ASSERT_TRUE(brought.ok) << brought.error.code << ": " << brought.error.message;
    EXPECT_EQ(brought.value.at("kind"), "classifier");
    EXPECT_TRUE(brought.value.at("standalone").get<bool>());
    EXPECT_FALSE(brought.value.at("features").empty());
    const Json listed = models_named("partner churn");
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_TRUE(listed[0].at("standalone").get<bool>());
    EXPECT_EQ(listed[0].at("metrics").at("held_out_auc"), 0.81);
    EXPECT_EQ(listed[0].at("feature_schema").at("source"), "drive:models/partner-churn.json");

    // Scored and served like a trained one.
    const ApiResult scored = call("score", {{"model", brought.value.at("model_id")},
                                            {"dataset_path", "crm.accounts"},
                                            {"top_k", 3}});
    ASSERT_TRUE(scored.ok) << scored.error.message;
    EXPECT_EQ(scored.value.at("ranked").size(), 3u);
    EXPECT_TRUE(call("expose_model", {{"model", brought.value.at("model_id")}}).ok);

    // A new version of it.
    const ApiResult version = call(
        "register_model", {{"version_of", brought.value.at("model_id")}, {"weights", weights}});
    ASSERT_TRUE(version.ok) << version.error.message;
    EXPECT_EQ(version.value.at("project_id"), brought.value.at("project_id"));
    EXPECT_EQ(models_named("partner churn").size(), 2u);

    // There is nothing to retrain in a brought model.
    EXPECT_EQ(call("retrain_model", {{"model_id", brought.value.at("model_id")}}).error.code,
              "not_supported");
    // A format the engine cannot read is refused, and leaves nothing.
    const ApiResult onnx = call("register_model", {{"name", "other"}, {"weights", "ONNX\x01"}});
    EXPECT_EQ(onnx.error.code, "not_supported");
    const ApiResult json = call("register_model", {{"name", "other"}, {"weights", "{\"a\":1}"}});
    EXPECT_EQ(json.error.code, "not_supported");
    EXPECT_TRUE(models_named("other").empty());
    EXPECT_FALSE(node.find_project("other (standalone model)"));
}

std::string model_fixture(const std::string& name) {
    std::ifstream in(std::filesystem::path(ZETTA_TEST_FIXTURES_DIR) / "model_import" / name,
                     std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// lake#681: ONNX, XGBoost and LightGBM files are brought like the engine's own, and any
// model's weights are downloaded, as kept or as ONNX.
TEST_F(StandaloneModels, BringCommonFormatsAndDownloadWeights) {
    // The fixtures read tenure, spend, tickets and age; name them as crm.accounts does.
    const Json columns = {"tenure_months", "monthly_spend", "support_tickets", "failed_payments"};
    const ApiResult onnx = call("register_model",
                                {{"name", "lightgbm churn"},
                                 {"weights_base64", zetta::detail::base64_encode_bytes(
                                                        model_fixture("lightgbm_binary_onnx.onnx"))},
                                 {"feature_names", columns}});
    ASSERT_TRUE(onnx.ok) << onnx.error.code << ": " << onnx.error.message;
    EXPECT_EQ(onnx.value.at("format"), "onnx");
    EXPECT_EQ(onnx.value.at("kind"), "classifier");
    EXPECT_EQ(onnx.value.at("features"), columns);
    EXPECT_EQ(models_named("lightgbm churn").at(0).at("feature_schema").at("format"), "onnx");
    const ApiResult scored = call("score", {{"model", onnx.value.at("model_id")},
                                            {"dataset_path", "crm.accounts"},
                                            {"top_k", 3}});
    ASSERT_TRUE(scored.ok) << scored.error.message;
    EXPECT_EQ(scored.value.at("ranked").size(), 3u);

    const ApiResult lightgbm = call("register_model", {{"name", "spend forecast"},
                                                       {"weights", model_fixture("lightgbm_regression.txt")},
                                                       {"feature_names", columns}});
    ASSERT_TRUE(lightgbm.ok) << lightgbm.error.message;
    EXPECT_EQ(lightgbm.value.at("format"), "lightgbm");
    EXPECT_EQ(lightgbm.value.at("kind"), "regressor");
    const ApiResult ubjson = call(
        "register_model",
        {{"version_of", onnx.value.at("model_id")},
         {"weights_base64", zetta::detail::base64_encode_bytes(model_fixture("xgboost_binary_ubj.ubj"))},
         {"feature_names", columns}});
    ASSERT_TRUE(ubjson.ok) << ubjson.error.message;
    EXPECT_EQ(ubjson.value.at("format"), "xgboost_ubjson");

    // Refused, naming why, and nothing kept.
    const ApiResult pickle = call("register_model",
                                  {{"name", "pickled"},
                                   {"weights_base64", zetta::detail::base64_encode_bytes(
                                                          std::string("\x80\x04\x95\x10sklearn", 11))}});
    EXPECT_EQ(pickle.error.code, "not_supported");
    EXPECT_NE(pickle.error.message.find("pickle"), std::string::npos);
    const ApiResult forest = call("register_model",
                                  {{"name", "forest"},
                                   {"weights_base64", zetta::detail::base64_encode_bytes(
                                                          model_fixture("sklearn_rfc.onnx"))}});
    EXPECT_EQ(forest.error.code, "not_supported");
    EXPECT_TRUE(models_named("pickled").empty());
    EXPECT_TRUE(models_named("forest").empty());
    EXPECT_EQ(call("register_model", {{"name", "x"}}).error.code, "invalid_input");
    EXPECT_EQ(call("register_model", {{"name", "x"}, {"weights", "{}"}, {"weights_base64", "e30="}})
                  .error.code,
              "invalid_input");

    // Download: as the engine keeps it, and as ONNX that reads back to the same model.
    const ApiResult kept = call("export_model", {{"model_id", onnx.value.at("model_id")}});
    ASSERT_TRUE(kept.ok) << kept.error.message;
    EXPECT_EQ(kept.value.at("encoding"), "utf-8");
    EXPECT_EQ(kept.value.at("media_type"), "application/json");
    const auto stored = node.find_model(onnx.value.at("model_id"));
    ASSERT_TRUE(stored);
    EXPECT_EQ(kept.value.at("content"), stored->read_weights().value());
    EXPECT_EQ(kept.value.at("features"), columns);
    const ApiResult as_onnx =
        call("export_model", {{"model_id", onnx.value.at("model_id")}, {"format", "onnx"}});
    ASSERT_TRUE(as_onnx.ok) << as_onnx.error.message;
    EXPECT_EQ(as_onnx.value.at("encoding"), "base64");
    EXPECT_TRUE(as_onnx.value.at("file_name").get<std::string>().ends_with(".onnx"));
    const std::string file =
        zetta::detail::base64_decode_to_string(as_onnx.value.at("content").get<std::string>());
    EXPECT_EQ(as_onnx.value.at("bytes"), file.size());
    std::string error;
    const auto reread = zetta::ml::import_model(file, {}, &error);
    ASSERT_TRUE(reread) << error;
    const auto original = zetta::ml::GbdtModel::from_json(Json::parse(stored->read_weights().value()));
    ASSERT_TRUE(original);
    EXPECT_EQ(reread->model.feature_names, original->feature_names);
    for (const std::vector<double>& row : {std::vector<double>{3, 120.5, 4, 0}, {40, 60, 0, 1}}) {
        EXPECT_DOUBLE_EQ(reread->model.predict_proba(row), original->predict_proba(row));
    }
    EXPECT_EQ(call("export_model", {{"model_id", "no-such-model"}}).error.code, "not_found");
}

TEST_F(StandaloneModels, MoveIntoAProjectShowsItWithItsVersions) {
    const std::string project_id = train_churn();
    const std::string model_id = models_named("churn").at(0).at("id");

    const ApiResult moved =
        call("move_model_to_project", {{"model_id", model_id}, {"project_name", "Retention"}});
    ASSERT_TRUE(moved.ok) << moved.error.message;
    EXPECT_EQ(moved.value.at("project_id"), project_id);
    EXPECT_EQ(moved.value.at("versions"), 1);
    EXPECT_EQ(project_names(), Json::array({"Retention"}));
    const Json model = models_named("churn").at(0);
    EXPECT_FALSE(model.at("standalone").get<bool>());
    EXPECT_EQ(model.at("project_name"), "Retention");

    // It is a project's model now: not moved twice, still retrainable.
    EXPECT_EQ(call("move_model_to_project", {{"model_id", model_id}}).error.code,
              "invalid_input");
    const ApiResult retrained = call("retrain_model", {{"model_id", model_id}});
    ASSERT_TRUE(retrained.ok) << retrained.error.message;
    EXPECT_FALSE(retrained.value.at("standalone").get<bool>());
    EXPECT_EQ(finish(retrained.value.at("run_id")), zetta::pipeline::RunStatus::Succeeded);
}

TEST_F(StandaloneModels, DeletingTheLastVersionRemovesItsProject) {
    const std::string project_id = train_churn();
    const ApiResult retrained =
        call("retrain_model", {{"model_id", models_named("churn").at(0).at("id")}});
    ASSERT_TRUE(retrained.ok) << retrained.error.message;
    ASSERT_EQ(finish(retrained.value.at("run_id")), zetta::pipeline::RunStatus::Succeeded);
    const Json versions = models_named("churn");
    ASSERT_EQ(versions.size(), 2u);

    const ApiResult first = call("delete_model", {{"model_id", versions[0].at("id")}});
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_FALSE(first.value.at("removed").contains("standalone_project"));
    EXPECT_TRUE(node.project_by_id(project_id));

    const ApiResult preview =
        call("delete_model", {{"model_id", versions[1].at("id")}, {"dry_run", true}});
    ASSERT_TRUE(preview.ok);
    EXPECT_EQ(preview.value.at("removed").at("standalone_project"), 1);
    EXPECT_TRUE(node.project_by_id(project_id));
    const ApiResult last = call("delete_model", {{"model_id", versions[1].at("id")}});
    ASSERT_TRUE(last.ok) << last.error.message;
    EXPECT_EQ(last.value.at("removed").at("standalone_project"), 1);
    EXPECT_FALSE(node.project_by_id(project_id));
    EXPECT_TRUE(models_named("churn").empty());
    // The runs are records and stay.
    EXPECT_TRUE(node.find_run(retrained.value.at("run_id")));
}

TEST(StandaloneProjects, TheInternalFlagReplicatesAndSurvivesARestart) {
    zetta::test::TempDir dir;
    std::string id;
    {
        zetta::cluster::ReplicationManager replication{"a"};
        zetta::NodeConfig config;
        config.data_dir = dir.path().string();
        config.preflight = false;
        zetta::Node node{config, &replication};
        ASSERT_TRUE(node.start());
        const auto project = node.create_project("m (standalone model)", "", "", true);
        ASSERT_TRUE(project);
        id = project->id();
        EXPECT_TRUE(node.project_names().empty());
        EXPECT_TRUE(replication.snapshot().at("project:" + id).value("internal", false));
        node.stop();
    }
    zetta::NodeConfig config;
    config.data_dir = dir.path().string();
    config.preflight = false;
    zetta::Node node{config};
    ASSERT_TRUE(node.start());
    const auto project = node.project_by_id(id);
    ASSERT_TRUE(project);
    EXPECT_TRUE(project->internal());
    EXPECT_TRUE(node.project_names().empty());
    EXPECT_TRUE(node.reveal_project(id, "Shown"));
    EXPECT_EQ(node.project_names(), std::vector<std::string>({"Shown"}));
    EXPECT_FALSE(node.reveal_project(id, "Again"));
    node.stop();
}

}  // namespace
