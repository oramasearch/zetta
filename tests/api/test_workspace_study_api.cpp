// Experiments over a workspace methodology's parameters (lake#689) through the
// registry, against a replicated node: list_methodologies publishes the
// declared parameters as a study search space once the methodology passed
// validation; create_study admits trials that wait for Lake's runner (the
// native driver never trains them); record_methodology_run completes a trial
// with its held-out objective and admits the next; stop_trial with a reason
// fails one; two failing the same way fail the study; a complete trial's model
// is promotable.
#include <gtest/gtest.h>

#include <string>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/ml/study.hpp"
#include "zetta/ml/workspace_study.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/run.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

struct WorkspaceStudyFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{"workspace-study-node"};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false},
                     &replication};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
        std::vector<nlohmann::json> rows;
        for (int index = 0; index < 40; ++index) {
            rows.push_back({{"customer_id", "c-" + std::to_string(index)},
                            {"logins", index % 9},
                            {"churned", index % 3 == 0 ? 1 : 0}});
        }
        zetta::data::AddDatasetOptions options;
        options.database = "shop";
        options.table_name = "customers";
        options.partitioned = false;
        const zetta::data::AddDatasetResult added =
            zetta::data::add_dataset_rows(node, rows, options);
        ASSERT_TRUE(added.ok()) << added.error;
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    }

    static nlohmann::json methodology() {
        return {{"name", "Gradient boosted churn"},
                {"input_kinds", {"tabular"}},
                {"target", {{"required", true}, {"kind", "binary"}}},
                {"parameters",
                 {{{"name", "learning_rate"}, {"kind", "float"}, {"default", 0.1},
                   {"minimum", 0.001}, {"maximum", 1.0}, {"log", true}},
                  {{"name", "max_depth"}, {"kind", "int"}, {"default", 6},
                   {"minimum", 1}, {"maximum", 16}},
                  {{"name", "balanced"}, {"kind", "bool"}, {"default", false}},
                  {{"name", "seed_only"}, {"kind", "int"}, {"default", 7}}}},
                {"metrics", {"auc", "log_loss"}},
                {"code", {{"repository_id", "repo-1"}, {"revision", "rev-a"}, {"path", "."}}}};
    }

    std::string registered(bool validated) {
        const ApiResult created = call("register_methodology", methodology());
        EXPECT_TRUE(created.ok) << created.error.message;
        const std::string id = created.value.at("methodology").at("id").get<std::string>();
        if (validated) {
            const ApiResult passed =
                call("update_methodology",
                     {{"id", id},
                      {"validation",
                       {{"status", "passed"},
                        {"findings", nlohmann::json::array()},
                        {"revision", "rev-a"}}}});
            EXPECT_TRUE(passed.ok) << passed.error.message;
        }
        return id;
    }

    ApiResult create(const std::string& id, nlohmann::json extra = nlohmann::json::object()) {
        nlohmann::json input = {{"name", "churn tuning"},
                                {"methodology", id},
                                {"dataset", "shop.customers"},
                                {"target", "churned"},
                                {"from_defaults", true},
                                {"budgets",
                                 {{"max_trials", 3},
                                  {"max_parallel_trials", 2},
                                  {"max_wall_time_ms", 0},
                                  {"cost_cap", 0.0}}},
                                {"sampler", "random"},
                                {"seed", 11}};
        for (const auto& [key, value] : extra.items()) input[key] = value;
        return call("create_study", input);
    }

    ApiResult record(const std::string& id, const nlohmann::json& work, double auc) {
        return call("record_methodology_run",
                    {{"methodology", id},
                     {"run_id", "lake-" + work.at("trial").get<std::string>()},
                     {"mode", "train"},
                     {"dataset_path", work.at("dataset_path")},
                     {"params", work.at("params")},
                     {"metrics", {{"in_sample_auc", auc + 0.05}, {"held_out_auc", auc}}},
                     {"rows", {{"train", 32}, {"held_out", 8}}},
                     {"artifact_base64", "YXJ0aWZhY3Q="},
                     {"study", work.at("study")},
                     {"trial", work.at("trial")},
                     {"duration_seconds", 12.5}});
    }
};

TEST_F(WorkspaceStudyFixture, AValidatedMethodologyPublishesItsParametersForStudies) {
    const std::string id = registered(false);
    ApiResult got = call("get_methodology", {{"id", id}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_FALSE(got.value.at("methodology").at("studies").get<bool>());
    EXPECT_NE(got.value.at("methodology").at("study_reason").get<std::string>().find("Validate"),
              std::string::npos);
    const ApiResult refused = create(id);
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.path, "/methodology");

    const std::string runnable = registered(true);
    const ApiResult listed = call("list_methodologies", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    nlohmann::json entry;
    for (const nlohmann::json& item : listed.value.at("methodologies")) {
        if (item.at("id") == runnable) entry = item;
    }
    ASSERT_TRUE(entry.is_object());
    EXPECT_TRUE(entry.at("studies").get<bool>());
    const nlohmann::json& space = entry.at("study_search_space");
    // seed_only has no range, so it is not searched.
    ASSERT_EQ(space.at("parameters").size(), 3u);
    EXPECT_EQ(space.at("parameters")[0].at("name"), "learning_rate");
    EXPECT_TRUE(space.at("parameters")[0].at("log").get<bool>());
    EXPECT_EQ(space.at("parameters")[1].at("kind"), "int");
    EXPECT_EQ(space.at("parameters")[2].at("choices"), nlohmann::json::array({false, true}));
    EXPECT_EQ(space.at("objective_metrics"),
              nlohmann::json::array({"held_out_auc", "held_out_log_loss"}));
    EXPECT_EQ(space.at("validation_splits"), nlohmann::json::array({"validation"}));
}

TEST_F(WorkspaceStudyFixture, TrialsWaitForLakesRunnerAndItsResultsAdmitTheNext) {
    const std::string id = registered(true);
    const ApiResult created = create(id);
    ASSERT_TRUE(created.ok) << created.error.message;
    const nlohmann::json& study = created.value;
    EXPECT_EQ(study.at("spec").at("objective").at("metric"), "held_out_auc");
    EXPECT_EQ(study.at("spec").at("objective").at("direction"), "maximize");
    EXPECT_EQ(study.at("spec").at("pruner"), "none");
    EXPECT_EQ(study.at("spec").at("target"), "churned");
    ASSERT_EQ(study.at("trials").size(), 2u);
    ASSERT_EQ(study.at("runner").size(), 2u);
    const nlohmann::json first = study.at("runner")[0];
    EXPECT_EQ(first.at("methodology"), id);
    EXPECT_EQ(first.at("dataset_path"), "shop.customers");
    EXPECT_EQ(first.at("target"), "churned");
    EXPECT_DOUBLE_EQ(first.at("holdout_fraction").get<double>(), 0.2);
    // The declared defaults fill what is not searched.
    EXPECT_EQ(first.at("params").at("seed_only"), 7);
    EXPECT_TRUE(first.at("params").contains("learning_rate"));
    const std::string study_id = study.at("id").get<std::string>();

    // The native driver never trains a workspace methodology's trials.
    node.tick();
    ApiResult got = call("get_study", {{"study", study_id}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value.at("status"), "running");
    EXPECT_EQ(got.value.at("runner").size(), 2u);
    const auto run = node.find_run(first.at("trial").get<std::string>());
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Running);

    // A result completes its trial and admits the third.
    const ApiResult recorded = record(id, first, 0.81);
    ASSERT_TRUE(recorded.ok) << recorded.error.message;
    const nlohmann::json& outcome = recorded.value.at("study");
    EXPECT_EQ(outcome.at("trial_state"), "complete");
    EXPECT_DOUBLE_EQ(outcome.at("objective").get<double>(), 0.81);
    EXPECT_EQ(outcome.at("status"), "running");
    ASSERT_EQ(outcome.at("runner").size(), 2u);
    EXPECT_EQ(recorded.value.at("run_id"), first.at("trial"));
    EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);
    const auto model = node.find_model(recorded.value.at("model_id").get<std::string>());
    ASSERT_TRUE(model);
    EXPECT_EQ(model->name(), "churn-tuning-trial-0");

    // A second report of the same trial is refused.
    const ApiResult again = record(id, first, 0.9);
    ASSERT_FALSE(again.ok);
    EXPECT_EQ(again.error.code, "conflict");

    // A run that failed ends its trial with the reason.
    const nlohmann::json second = outcome.at("runner")[0];
    const ApiResult failed =
        call("stop_trial", {{"study", study_id},
                            {"run", second.at("trial")},
                            {"reason", "the methodology's code exited with 1"}});
    ASSERT_TRUE(failed.ok) << failed.error.message;
    EXPECT_EQ(failed.value.at("state"), "failed");
    EXPECT_EQ(failed.value.at("status"), "running");
    ASSERT_EQ(failed.value.at("runner").size(), 1u);

    // The last trial completes the study, and the best trial is promotable.
    const nlohmann::json third = failed.value.at("runner")[0];
    EXPECT_EQ(third.at("number"), 2);
    const ApiResult last = record(id, third, 0.86);
    ASSERT_TRUE(last.ok) << last.error.message;
    EXPECT_EQ(last.value.at("study").at("status"), "complete");
    EXPECT_TRUE(last.value.at("study").at("runner").empty());

    const ApiResult analysis = call("get_study_analysis", {{"study", study_id}});
    ASSERT_TRUE(analysis.ok) << analysis.error.message;
    EXPECT_EQ(analysis.value.at("best_trial").at("run"), third.at("trial"));
    const ApiResult promoted =
        call("promote_trial", {{"study", study_id}, {"run", third.at("trial")}});
    ASSERT_TRUE(promoted.ok) << promoted.error.message;
    EXPECT_EQ(promoted.value.at("model"), last.value.at("model_id"));
}

TEST_F(WorkspaceStudyFixture, TwoTrialsFailingTheSameWayFailTheStudy) {
    const std::string id = registered(true);
    const ApiResult created = create(id);
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string study_id = created.value.at("id").get<std::string>();
    const nlohmann::json runner = created.value.at("runner");
    const std::string reason = "the methodology's code exited with 2";
    ASSERT_TRUE(call("stop_trial", {{"study", study_id},
                                    {"run", runner[0].at("trial")},
                                    {"reason", reason}})
                    .ok);
    const ApiResult second = call(
        "stop_trial", {{"study", study_id}, {"run", runner[1].at("trial")}, {"reason", reason}});
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(second.value.at("status"), "failed");
    EXPECT_TRUE(second.value.at("runner").empty());
    const ApiResult got = call("get_study", {{"study", study_id}});
    ASSERT_TRUE(got.ok);
    EXPECT_NE(got.value.at("failure_reason").get<std::string>().find(reason), std::string::npos);
}

TEST_F(WorkspaceStudyFixture, ASearchSpaceIsCheckedAgainstTheDeclaredParameters) {
    const std::string id = registered(true);
    const nlohmann::json outside = {
        {"search_space",
         {{{"name", "max_depth"}, {"kind", "int"}, {"minimum", 1}, {"maximum", 40},
           {"step", 1}, {"log", false}, {"choices", nlohmann::json::array()},
           {"condition_parameter", ""}, {"condition_values", nlohmann::json::array()}}}}};
    ApiResult refused = create(id, outside);
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.path, "/search_space/max_depth/maximum");

    refused = create(id, {{"objective",
                           {{"metric", "held_out_r2"},
                            {"direction", "maximize"},
                            {"validation_split", "validation"}}}});
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.path, "/objective/metric");

    // log_loss is minimized when named without a direction's default.
    const ApiResult loss =
        create(id, {{"name", "loss tuning"},
                    {"objective",
                     {{"metric", "log_loss"}, {"direction", "minimize"},
                      {"validation_split", "validation"}}}});
    ASSERT_TRUE(loss.ok) << loss.error.message;
    EXPECT_EQ(loss.value.at("spec").at("objective").at("metric"), "held_out_log_loss");
    EXPECT_TRUE(zetta::ml::metric_is_minimized("held_out_log_loss"));
    EXPECT_FALSE(zetta::ml::metric_is_minimized("held_out_auc"));
}

}  // namespace
