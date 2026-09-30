#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/study.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"

namespace {

using Json = nlohmann::json;
using zetta::api::ApiContext;
using zetta::api::ApiResult;

std::vector<Json> churn_rows(std::size_t count) {
    std::vector<Json> rows;
    rows.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::int64_t failed_payments =
            static_cast<std::int64_t>(index % 5);
        const double logins = static_cast<double>(60 - (index % 55));
        const bool signal = failed_payments >= 3 && logins < 30.0;
        const bool flipped = index % 13 == 0;
        const unsigned day = 1 + static_cast<unsigned>(index % 28);
        std::string observed_at = "2026-08-";
        if (day < 10) observed_at += "0";
        observed_at += std::to_string(day) + "T00:00:00Z";
        rows.push_back(
            {{"customer_id", "customer-" + std::to_string(1000 + index)},
             {"observed_at", std::move(observed_at)},
             {"failed_payments", failed_payments},
             {"logins_30d", logins},
             {"nps", static_cast<double>(index % 11)},
             {"churned", signal != flipped ? 1 : 0}});
    }
    return rows;
}

std::vector<Json> monthly_churn_rows(bool positives_only_in_last_month) {
    std::vector<Json> rows;
    rows.reserve(12000);
    for (std::size_t month = 1; month <= 6; ++month) {
        for (std::size_t customer = 0; customer < 2000; ++customer) {
            const bool positive_customer = customer % 6 == 0;
            const bool positive = positive_customer &&
                                  (!positives_only_in_last_month || month == 6);
            std::string observed_at = "2026-0" + std::to_string(month) +
                                      "-01T00:00:00Z";
            rows.push_back(
                {{"customer_id", "customer-" + std::to_string(customer)},
                 {"observed_at", std::move(observed_at)},
                 {"usage", positive_customer ? 4.0 : 90.0},
                 {"complaints", positive_customer ? 9.0 : 0.0},
                 {"churned", positive ? 1 : 0}});
        }
    }
    return rows;
}

Json integer_parameter(std::string name, int minimum, int maximum,
                       int step) {
    maximum = std::max(maximum, minimum + 1);
    return {{"name", std::move(name)},
            {"kind", "int"},
            {"minimum", minimum},
            {"maximum", maximum},
            {"step", step},
            {"log", false},
            {"choices", Json::array()},
            {"condition_parameter", ""},
            {"condition_values", Json::array()}};
}

Json study_input(std::string name, std::size_t max_trials,
                 int minimum_trees, int maximum_trees) {
    return {
        {"name", std::move(name)},
        {"dataset", "study_data.churn"},
        {"methodology", "M-1"},
        {"objective",
         {{"metric", "validation.auc"},
          {"direction", "maximize"},
          {"validation_split", "validation"}}},
        {"search_space",
         Json::array(
             {integer_parameter("gbdt.num_trees", minimum_trees,
                                maximum_trees,
                                std::max(1, maximum_trees - minimum_trees)),
              integer_parameter("gbdt.max_depth", 2, 2, 1),
              integer_parameter("gbdt.max_bins", 8, 8, 1)})},
        {"sampler", "grid"},
        {"pruner", "none"},
        {"budgets",
         {{"max_trials", max_trials},
          {"max_wall_time_ms", 0},
          {"max_parallel_trials", 1},
          {"cost_cap", 0.0}}},
        {"seed", 1205}};
}

class StudyDriverJourney : public ::testing::Test {
protected:
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{"study-driver-node"};
    zetta::Node node{node_config(), &replication};
    zetta::cluster::Cluster cluster{"study-driver-node", cluster_config(),
                                    &replication};
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
        config.listen_port = 24831;
        config.port_range = 4;
        return config;
    }

    void SetUp() override {
        ASSERT_TRUE(node.start());
        replication.set(
            "node:" + node.node_id(),
            {{"address", "127.0.0.1:24831"},
             {"reachability", "wan-accessible"},
             {"node_type", "permanent"}});
        cluster.start();
        node.attach_cluster(cluster, 2);
        zetta::api::register_core_capabilities(registry);
        node.tick();
        ASSERT_EQ(node.cluster_members(),
                  std::vector<std::string>{node.node_id()});

        zetta::data::AddDatasetOptions options;
        options.database = "study_data";
        options.table_name = "churn";
        options.partitioned = true;
        options.members = {node.node_id()};
        const zetta::data::AddDatasetResult added =
            zetta::data::add_dataset_rows(node, churn_rows(160), options);
        ASSERT_TRUE(added.ok()) << added.error;
        ASSERT_TRUE(added.table);
    }

    void TearDown() override {
        background.prune();
        node.stop();
        cluster.stop();
    }

    ApiResult call(const std::string& name, const Json& input) {
        ApiContext context{node, &cluster, {}, {}, &node.span_recorder(),
                           &background};
        return registry.call(name, input, context);
    }

    void land_table(std::string table, std::vector<Json> rows) {
        zetta::data::AddDatasetOptions options;
        options.database = "study_data";
        options.table_name = std::move(table);
        options.partitioned = true;
        options.members = {node.node_id()};
        const zetta::data::AddDatasetResult added =
            zetta::data::add_dataset_rows(node, std::move(rows), options);
        ASSERT_TRUE(added.ok()) << added.error;
        ASSERT_TRUE(added.table);
    }

    bool pump_until(const std::function<bool()>& ready,
                    std::size_t attempts = 30000) {
        for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
            node.tick();
            background.prune();
            if (ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
};

TEST_F(StudyDriverJourney,
       DatasetStudyResumesAndProducesRunsCurvesAnalysisAndPromotion) {
    const ApiResult created =
        call("create_study", study_input("dataset study", 3, 8, 10));
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string study_id = created.value.at("id");

    ASSERT_TRUE(pump_until([&] {
        const auto study = node.find_study(study_id);
        return study && !study->value().trials.empty() &&
               study->value().trials.front().state ==
                   zetta::ml::TrialState::Running;
    }));
    const std::string first_run =
        node.find_study(study_id)->value().trials.front().run;

    node.stop();
    ASSERT_TRUE(node.start());
    node.tick();

    ASSERT_TRUE(pump_until([&] {
        const auto study = node.find_study(study_id);
        return study && study->status() == zetta::ml::StudyStatus::Complete;
    }));

    const auto study = node.find_study(study_id);
    ASSERT_TRUE(study);
    EXPECT_EQ(study->value().stop_reason, "max_trials");
    ASSERT_EQ(study->value().trials.size(), 3u);
    EXPECT_NE(std::ranges::find(study->value().trials, first_run,
                                &zetta::ml::StudyTrial::run),
              study->value().trials.end());

    std::vector<const zetta::ml::StudyTrial*> complete;
    for (const zetta::ml::StudyTrial& trial : study->value().trials) {
        const auto run = node.find_run(trial.run);
        ASSERT_TRUE(run);
        EXPECT_EQ(run->value().metrics.at("params").at("study.id"), study_id);
        EXPECT_EQ(run->value().metrics.at("params").at("study.seed"),
                  trial.seed);
        if (trial.state != zetta::ml::TrialState::Complete) continue;
        complete.push_back(&trial);
        EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);
        EXPECT_TRUE(trial.objective.has_value());
        EXPECT_FALSE(trial.intermediate.empty());
        EXPECT_FALSE(trial.model.empty());
        const auto model = node.find_model(trial.model);
        ASSERT_TRUE(model);
        EXPECT_EQ(model->run_id(), trial.run);
    }
    ASSERT_GE(complete.size(), 2u);
    EXPECT_EQ(node.models().size(), complete.size());

    const ApiResult events =
        call("receive_events", {{"topic", "study-events." + study_id}});
    ASSERT_TRUE(events.ok) << events.error.message;
    std::set<std::string> event_names;
    for (const Json& event : events.value.at("events")) {
        event_names.insert(
            Json::parse(event.at("payload").get<std::string>()).at("event"));
    }
    EXPECT_TRUE(event_names.contains("trial_started"));
    EXPECT_TRUE(event_names.contains("trial_completed"));
    EXPECT_TRUE(event_names.contains("study_complete"));

    const ApiResult analysis = call("get_study_analysis", {{"study", study_id}});
    ASSERT_TRUE(analysis.ok) << analysis.error.message;
    ASSERT_FALSE(analysis.value.at("best_trial").is_null());
    EXPECT_TRUE(analysis.value.at("best_trial").at("value").is_number());
    EXPECT_GE(analysis.value.at("optimization_history").size(), 2u);

    const ApiResult promoted =
        call("promote_trial",
             {{"study", study_id}, {"run", complete.front()->run}});
    ASSERT_TRUE(promoted.ok) << promoted.error.message;
    EXPECT_EQ(promoted.value.at("model"), complete.front()->model);
    EXPECT_EQ(study->value().promoted_model, complete.front()->model);
}

TEST_F(StudyDriverJourney, PipelineStudyRunsTheStoredGraph) {
    const auto project = node.create_project("study project");
    ASSERT_TRUE(project);
    zetta::ir::ExecutionGraph graph;
    graph.add_node(zetta::ir::StepKind::Connect, "Read churn table",
                   {{"source", "study_data"}, {"table", "churn"}},
                   "read_churn");
    graph.add_node(zetta::ir::StepKind::Clean, "Build features", {},
                   "build_features");
    graph.add_node(zetta::ir::StepKind::Model, "Train GBDT",
                   {{"model_kind", "gbdt"}, {"target", "churned"}},
                   "train_gbdt");
    graph.add_edge("read_churn", "build_features");
    graph.add_edge("build_features", "train_gbdt");
    const auto pipeline =
        project->create_graph("study pipeline", std::move(graph));
    ASSERT_TRUE(pipeline);

    Json input = study_input("pipeline study", 2, 4, 6);
    input["project"] = project->id();
    input["pipeline"] = pipeline->id();
    const ApiResult created = call("create_study", input);
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string study_id = created.value.at("id");

    ASSERT_TRUE(pump_until([&] {
        const auto study = node.find_study(study_id);
        return study && study->status() == zetta::ml::StudyStatus::Complete;
    }));
    const auto study = node.find_study(study_id);
    ASSERT_TRUE(study);
    ASSERT_EQ(study->value().trials.size(), 2u);
    for (const zetta::ml::StudyTrial& trial : study->value().trials) {
        EXPECT_EQ(trial.state, zetta::ml::TrialState::Complete);
        const auto run = node.find_run(trial.run);
        ASSERT_TRUE(run);
        EXPECT_EQ(run->project(), project->id());
        EXPECT_EQ(run->graph(), pipeline->id());
        EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);
    }
}

TEST_F(StudyDriverJourney, StopStudyFailsItsActiveTrainingRun) {
    const ApiResult created =
        call("create_study", study_input("stopped study", 4, 30, 32));
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string study_id = created.value.at("id");
    ASSERT_TRUE(pump_until([&] {
        const auto study = node.find_study(study_id);
        return study && !study->value().trials.empty() &&
               study->value().trials.front().state ==
                   zetta::ml::TrialState::Running;
    }));
    const std::string run_id =
        node.find_study(study_id)->value().trials.front().run;

    const ApiResult stopped = call("stop_study", {{"study", study_id}});
    ASSERT_TRUE(stopped.ok) << stopped.error.message;
    EXPECT_EQ(stopped.value.at("status"), "stopped");
    const auto study = node.find_study(study_id);
    ASSERT_TRUE(study);
    EXPECT_EQ(study->status(), zetta::ml::StudyStatus::Stopped);
    EXPECT_EQ(study->value().stop_reason, "stopped");
    ASSERT_FALSE(study->value().trials.empty());
    EXPECT_EQ(study->value().trials.front().state,
              zetta::ml::TrialState::Failed);
    const auto run = node.find_run(run_id);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Failed);
    const ApiResult events =
        call("receive_events", {{"topic", "study-events." + study_id}});
    ASSERT_TRUE(events.ok) << events.error.message;
    EXPECT_TRUE(std::ranges::any_of(
        events.value.at("events"), [](const Json& event) {
            const Json payload =
                Json::parse(event.at("payload").get<std::string>());
            return payload.at("event") == "trial_failed";
        }));
}

TEST_F(StudyDriverJourney, WallAndCostBudgetsStopAdmission) {
    Json wall_input = study_input("wall budget", 3, 5000, 5000);
    wall_input["budgets"]["max_wall_time_ms"] = 50;
    const ApiResult wall_created = call("create_study", wall_input);
    ASSERT_TRUE(wall_created.ok) << wall_created.error.message;
    const std::string wall_study_id = wall_created.value.at("id");
    ASSERT_TRUE(pump_until([&] {
        const auto study = node.find_study(wall_study_id);
        return study && !study->value().trials.empty();
    }));
    ASSERT_TRUE(pump_until([&] {
        const auto study = node.find_study(wall_study_id);
        return study && study->status() == zetta::ml::StudyStatus::Complete;
    }));
    const auto wall_study = node.find_study(wall_study_id);
    ASSERT_TRUE(wall_study);
    EXPECT_EQ(wall_study->value().stop_reason, "wall_time");
    ASSERT_EQ(wall_study->value().trials.size(), 1u);
    EXPECT_EQ(wall_study->value().trials.front().state,
              zetta::ml::TrialState::Failed);
    const auto wall_run =
        node.find_run(wall_study->value().trials.front().run);
    ASSERT_TRUE(wall_run);
    EXPECT_EQ(wall_run->status(), zetta::pipeline::RunStatus::Failed);

    // A cap below one trial's first reservation (60 trial-seconds) can never admit a trial, so
    // it is rejected at validation instead of completing with zero trials (live 2026-09-30:
    // an agent proposed cost_cap 5).
    Json cost_input = study_input("cost budget", 3, 4, 4);
    cost_input["budgets"]["cost_cap"] = 59.0;
    const ApiResult cost_validated = call("validate_study", cost_input);
    ASSERT_TRUE(cost_validated.ok) << cost_validated.error.message;
    EXPECT_FALSE(cost_validated.value.at("valid").get<bool>());
    EXPECT_NE(cost_validated.value.at("reason").get<std::string>().find("cannot admit one trial"),
              std::string::npos)
        << cost_validated.value.dump();
    EXPECT_EQ(cost_validated.value.at("path").get<std::string>(), "/budgets/cost_cap")
        << cost_validated.value.dump();
    const ApiResult cost_created = call("create_study", cost_input);
    ASSERT_FALSE(cost_created.ok);
    EXPECT_NE(cost_created.error.message.find("cannot admit one trial"), std::string::npos)
        << cost_created.error.message;
}

TEST_F(StudyDriverJourney, RejectsAnUnlearnableOutOfTimeSplitBeforeTrials) {
    land_table("last_month_churn", monthly_churn_rows(true));
    Json input = study_input("unlearnable split", 4, 4, 6);
    input["dataset"] = "study_data.last_month_churn";
    input["budgets"]["max_parallel_trials"] = 2;
    input["budgets"]["cost_cap"] = 300.0;

    const ApiResult validated = call("validate_study", input);
    ASSERT_TRUE(validated.ok) << validated.error.message;
    EXPECT_FALSE(validated.value.at("valid").get<bool>());
    EXPECT_EQ(validated.value.at("path"), "/dataset");
    EXPECT_NE(validated.value.at("reason").get<std::string>().find(
                  "out-of-time training split has 0 positive labels"),
              std::string::npos);
    EXPECT_NE(validated.value.at("reason").get<std::string>().find(
                  "positives only appear after 2026-06-01T00:00:00Z"),
              std::string::npos);

    zetta::ml::StudySpec spec;
    spec.name = "driver preflight failure";
    spec.dataset = "study_data.last_month_churn";
    spec.methodology = "M-1";
    spec.objective.metric = "validation.auc";
    spec.objective.direction = zetta::ml::StudyDirection::Maximize;
    spec.objective.validation_split = "validation";
    zetta::ml::StudyParameter trees;
    trees.name = "gbdt.num_trees";
    trees.kind = zetta::ml::ParameterKind::Integer;
    trees.minimum = 4.0;
    trees.maximum = 6.0;
    trees.step = 2.0;
    spec.search_space = {std::move(trees)};
    spec.sampler = zetta::ml::SamplerKind::Grid;
    spec.pruner = zetta::ml::PrunerKind::None;
    spec.budgets.max_trials = 4;
    spec.budgets.max_parallel_trials = 2;
    spec.budgets.cost_cap = 300.0;
    spec.seed = 1209;
    const auto study = node.create_study(std::move(spec));
    ASSERT_TRUE(study);
    ASSERT_TRUE(pump_until([&] {
        return study->status() == zetta::ml::StudyStatus::Failed;
    }));
    EXPECT_EQ(study->value().stop_reason, "data");
    EXPECT_TRUE(study->value().trials.empty());
    EXPECT_NE(study->value().failure_reason.find(
                  "out-of-time training split has 0 positive labels"),
              std::string::npos);
}

TEST_F(StudyDriverJourney, LearnableMonthlyStudyUsesTheFullTable) {
    land_table("monthly_churn", monthly_churn_rows(false));
    Json input = study_input("learnable monthly study", 2, 8, 10);
    input["dataset"] = "study_data.monthly_churn";
    input["budgets"]["max_parallel_trials"] = 2;
    input["budgets"]["cost_cap"] = 300.0;

    const ApiResult validated = call("validate_study", input);
    ASSERT_TRUE(validated.ok) << validated.error.message;
    ASSERT_TRUE(validated.value.at("valid").get<bool>())
        << validated.value.at("reason");
    const ApiResult created = call("create_study", input);
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string study_id = created.value.at("id");
    ASSERT_TRUE(pump_until([&] {
        const auto study = node.find_study(study_id);
        return study && study->status() == zetta::ml::StudyStatus::Complete;
    }));

    const auto study = node.find_study(study_id);
    ASSERT_TRUE(study);
    ASSERT_EQ(study->value().trials.size(), 2u);
    EXPECT_LE(study->value().spent_cost, study->spec().budgets.cost_cap);
    for (const zetta::ml::StudyTrial& trial : study->value().trials) {
        ASSERT_EQ(trial.state, zetta::ml::TrialState::Complete)
            << trial.reason;
        ASSERT_TRUE(trial.objective.has_value());
        EXPECT_GT(*trial.objective, 0.7);
        const auto model = node.find_model(trial.model);
        ASSERT_TRUE(model);
        const Json metrics = model->metrics();
        EXPECT_EQ(metrics.at("dataset_rows"), 12000.0);
        EXPECT_EQ(metrics.at("in_sample_rows").get<double>() +
                      metrics.at("held_out_rows").get<double>(),
                  12000.0);
        EXPECT_GT(metrics.at("held_out_auc").get<double>(), 0.7);
    }
}

TEST_F(StudyDriverJourney, CreateRejectsUnknownParametersAndUnsupportedMethods) {
    Json alias = study_input("canonical family", 1, 4, 4);
    Json family = {{"name", "model_family"},
                   {"kind", "categorical"},
                   {"minimum", 0.0},
                   {"maximum", 1.0},
                   {"step", 0.0},
                   {"log", false},
                   {"choices", Json::array({"gradient_boosting"})},
                   {"condition_parameter", ""},
                   {"condition_values", Json::array()}};
    alias["search_space"].insert(alias["search_space"].begin(),
                                 std::move(family));
    const ApiResult alias_result = call("create_study", alias);
    ASSERT_TRUE(alias_result.ok) << alias_result.error.message;
    const auto alias_study =
        node.find_study(alias_result.value.at("id").get<std::string>());
    ASSERT_TRUE(alias_study);
    ASSERT_FALSE(alias_study->spec().search_space.empty());
    ASSERT_EQ(alias_study->spec().search_space.front().choices.size(), 1u);
    EXPECT_EQ(alias_study->spec().search_space.front().choices.front(),
              "gbdt");
    ASSERT_TRUE(call("stop_study", {{"study", alias_study->id()}}).ok);

    Json unknown = study_input("unknown parameter", 1, 4, 4);
    unknown["search_space"][0]["name"] = "unknown_control";
    const ApiResult unknown_result = call("create_study", unknown);
    EXPECT_FALSE(unknown_result.ok);
    EXPECT_NE(unknown_result.error.message.find("unknown M-1 hyperparameter"),
              std::string::npos);

    Json unsupported = study_input("unsupported method", 1, 4, 4);
    unsupported["methodology"] = "M-2";
    const ApiResult unsupported_result = call("create_study", unsupported);
    EXPECT_FALSE(unsupported_result.ok);
    EXPECT_NE(unsupported_result.error.message.find("currently supports M-1"),
              std::string::npos);

    Json wrong_split = study_input("wrong split", 1, 4, 4);
    wrong_split["objective"]["validation_split"] = "holdout";
    const ApiResult split_result = call("create_study", wrong_split);
    EXPECT_FALSE(split_result.ok);
    EXPECT_NE(split_result.error.message.find("supports the 'validation' split"),
              std::string::npos);
}

TEST_F(StudyDriverJourney, ValidateAndCreateUseTheSameNormalizedSpec) {
    struct ValidationCase {
        std::string name;
        std::function<void(Json&)> change;
    };
    const std::vector<ValidationCase> cases = {
        {"defaults", [](Json& input) {
             input.erase("search_space");
             input["from_defaults"] = true;
         }},
        {"family alias", [](Json& input) {
             Json family = {{"name", "model_family"},
                            {"kind", "categorical"},
                            {"minimum", 0.0},
                            {"maximum", 1.0},
                            {"step", 0.0},
                            {"log", false},
                            {"choices", Json::array({"gradient_boosting"})},
                            {"condition_parameter", ""},
                            {"condition_values", Json::array()}};
             input["search_space"].insert(input["search_space"].begin(),
                                           std::move(family));
         }},
        {"unsupported family", [](Json& input) {
             Json family = {{"name", "model_family"},
                            {"kind", "categorical"},
                            {"minimum", 0.0},
                            {"maximum", 1.0},
                            {"step", 0.0},
                            {"log", false},
                            {"choices", Json::array({"linear"})},
                            {"condition_parameter", ""},
                            {"condition_values", Json::array()}};
             input["search_space"].insert(input["search_space"].begin(),
                                           std::move(family));
         }},
        {"unknown parameter", [](Json& input) {
             input["search_space"][0]["name"] = "unknown_control";
         }},
        {"unsupported methodology", [](Json& input) {
             input["methodology"] = "M-2";
         }},
        {"wrong split", [](Json& input) {
             input["objective"]["validation_split"] = "holdout";
         }},
        {"directory dataset", [](Json& input) {
             input["dataset"] = "./study_data/churn";
         }},
    };

    for (std::size_t index = 0; index < cases.size(); ++index) {
        SCOPED_TRACE(cases[index].name);
        Json input = study_input(
            "validation parity " + std::to_string(index), 1, 4, 6);
        cases[index].change(input);
        const ApiResult validated = call("validate_study", input);
        ASSERT_TRUE(validated.ok) << validated.error.message;
        EXPECT_FALSE(node.find_study(input.at("name").get<std::string>()));
        const ApiResult created = call("create_study", input);
        EXPECT_EQ(validated.value.at("valid").get<bool>(), created.ok);
        if (created.ok) {
            EXPECT_EQ(validated.value.at("normalized_spec"),
                      created.value.at("spec"));
            EXPECT_TRUE(validated.value.at("reason").get<std::string>().empty());
            EXPECT_TRUE(validated.value.at("path").get<std::string>().empty());
            EXPECT_TRUE(call("stop_study", {{"study", created.value.at("id")}}).ok);
            continue;
        }
        EXPECT_TRUE(validated.value.at("normalized_spec").is_null());
        EXPECT_EQ(validated.value.at("reason"), created.error.message);
        EXPECT_EQ(validated.value.at("path"), created.error.path);
    }
}

TEST_F(StudyDriverJourney, PublishedStudySpaceBuildsAValidSpec) {
    const ApiResult listed =
        call("list_methodologies", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    const auto m1 = std::ranges::find_if(
        listed.value.at("methodologies"), [](const Json& methodology) {
            return methodology.at("id") == "M-1";
        });
    ASSERT_NE(m1, listed.value.at("methodologies").end());
    ASSERT_TRUE(m1->at("studies").get<bool>());
    const Json& published = m1->at("study_search_space");
    ASSERT_EQ(published.at("parameters").size(), 9u);
    const auto family = std::ranges::find(
        published.at("parameters"), "model_family",
        [](const Json& parameter) { return parameter.at("name"); });
    ASSERT_NE(family, published.at("parameters").end());
    EXPECT_EQ(family->at("kind"), "categorical");
    EXPECT_EQ(family->at("default"), "gbdt");
    EXPECT_EQ(family->at("choices"), Json::array({"gbdt"}));
    const auto trees = std::ranges::find(
        published.at("parameters"), "gbdt.num_trees",
        [](const Json& parameter) { return parameter.at("name"); });
    ASSERT_NE(trees, published.at("parameters").end());
    EXPECT_EQ(trees->at("kind"), "int");
    EXPECT_EQ(trees->at("minimum"), 10.0);
    EXPECT_EQ(trees->at("maximum"), 200.0);
    EXPECT_EQ(trees->at("default"), 100);
    const auto regularization = std::ranges::find(
        published.at("parameters"), "l2_regularization",
        [](const Json& parameter) { return parameter.at("name"); });
    ASSERT_NE(regularization, published.at("parameters").end());
    EXPECT_EQ(regularization->at("aliases"),
              Json::array({"gbdt.l2_regularization"}));
    EXPECT_EQ(published.at("objective_metrics"),
              Json::array({"validation.accuracy", "validation.auc",
                           "validation.log_loss"}));
    EXPECT_EQ(published.at("validation_splits"),
              Json::array({"validation"}));
    EXPECT_EQ(published.at("dataset_path").at("format"),
              "database.table");
    EXPECT_TRUE(published.at("dataset_path").at("must_exist"));
    EXPECT_FALSE(published.at("dataset_path").at("directory_paths"));

    Json input = study_input("published search space", 1, 4, 6);
    input["search_space"] = Json::array();
    for (Json parameter : published.at("parameters")) {
        parameter.erase("default");
        parameter.erase("aliases");
        input["search_space"].push_back(std::move(parameter));
    }
    const ApiResult validated = call("validate_study", input);
    ASSERT_TRUE(validated.ok) << validated.error.message;
    EXPECT_TRUE(validated.value.at("valid").get<bool>())
        << validated.value.at("path") << ": "
        << validated.value.at("reason");

    const auto m2 = std::ranges::find_if(
        listed.value.at("methodologies"), [](const Json& methodology) {
            return methodology.at("id") == "M-2";
        });
    ASSERT_NE(m2, listed.value.at("methodologies").end());
    EXPECT_FALSE(m2->at("studies").get<bool>());
    EXPECT_FALSE(m2->at("study_reason").get<std::string>().empty());
    EXPECT_TRUE(m2->at("study_search_space").is_null());
}

}  // namespace
