// #1216: the /docs Getting started path — upload a churn CSV, describe it,
// approve, check and run — trains end to end, and a Study trial over the same
// table selects the same features as the run.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <functional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/ml/study.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

namespace {

using Json = nlohmann::json;
using zetta::api::ApiContext;
using zetta::api::ApiResult;

constexpr std::string_view kMeasures[] = {"support_tickets", "tenure_months", "monthly_spend",
                                          "failed_payments"};

// The Getting started dataset, generated deterministically: 400 accounts whose
// churn follows tickets, failed payments, short tenure and the basic plan, with
// every 17th label flipped so the problem is not separable.
std::string getting_started_csv() {
    constexpr std::array<std::string_view, 3> plans = {"basic", "pro", "enterprise"};
    std::string csv = "account_id,support_tickets,tenure_months,monthly_spend,failed_payments,"
                      "plan,churned\n";
    for (int index = 0; index < 400; ++index) {
        const int support_tickets = (index * 7) % 11;
        const int tenure_months = 1 + (index * 13) % 72;
        const double monthly_spend = 19.0 + ((index * 37) % 400) * 0.75 + (index % 7) * 0.01;
        const int failed_payments = (index * 5) % 4;
        const std::string_view plan = plans[(index / 3) % 3];
        const double risk = 0.3 * support_tickets + 0.8 * failed_payments -
                            0.04 * tenure_months + (plan == "basic" ? 0.5 : 0.0);
        int churned = risk > 2.0 ? 1 : 0;
        if (index % 17 == 0) churned = 1 - churned;
        csv += std::format("{},{},{},{:.2f},{},{},{}\n", 5000 + index, support_tickets,
                           tenure_months, monthly_spend, failed_payments, plan, churned);
    }
    return csv;
}

Json integer_parameter(std::string name, int minimum, int maximum) {
    return {{"name", std::move(name)}, {"kind", "int"}, {"minimum", minimum},
            {"maximum", maximum},      {"step", 1},     {"log", false},
            {"choices", Json::array()}, {"condition_parameter", ""},
            {"condition_values", Json::array()}};
}

std::set<std::string> used_features(const Json& features) {
    std::set<std::string> names;
    for (const Json& feature : features.at("used")) {
        names.insert(feature.at("name").get<std::string>());
    }
    return names;
}

const Json* excluded(const Json& features, std::string_view name) {
    const auto& list = features.at("excluded");
    const auto found = std::ranges::find_if(
        list, [name](const Json& item) { return item.at("name") == name; });
    return found == list.end() ? nullptr : &*found;
}

class GettingStartedChurn : public ::testing::Test {
protected:
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{"getting-started-node"};
    zetta::Node node{node_config(), &replication};
    zetta::cluster::Cluster cluster{"getting-started-node", cluster_config(), &replication};
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
        config.listen_port = 24850;
        config.port_range = 4;
        return config;
    }

    void SetUp() override {
        ASSERT_TRUE(node.start());
        // The Study driver admits trials on a permanent, reachable member.
        replication.set("node:" + node.node_id(), {{"address", "127.0.0.1:24850"},
                                                   {"reachability", "wan-accessible"},
                                                   {"node_type", "permanent"}});
        cluster.start();
        node.attach_cluster(cluster, 2);
        ASSERT_NE(node.job_coordinator(), nullptr);
        zetta::api::register_core_capabilities(registry);
        node.tick();
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

    bool pump_until(const std::function<bool()>& ready, std::size_t attempts = 60000) {
        for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
            node.tick();
            background.prune();
            if (ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    void upload(const std::string& table, const std::string& csv) {
        const ApiResult uploaded = call("upload_dataset", {{"database", "getting_started"},
                                                           {"table_name", table},
                                                           {"format", "csv"},
                                                           {"content", csv}});
        ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    }

    // Describe it, Approve: the pipeline a customer builds from Getting started.
    void build_pipeline(const std::string& project) {
        ASSERT_TRUE(call("create_project", {{"name", project}}).ok);
        const ApiResult captured =
            call("capture_intent",
                 {{"project", project}, {"text", "Predict the churned column of accounts"}});
        ASSERT_TRUE(captured.ok) << captured.error.message;
        const ApiResult compiled =
            call("compile_intent", {{"project", project},
                                    {"input_id", captured.value.at("input_id")},
                                    {"methodology", "M-1"},
                                    {"name", "churn"}});
        ASSERT_TRUE(compiled.ok) << compiled.error.message;
        ASSERT_EQ(compiled.value.at("methodology"), "M-1");
        const ApiResult approved = call("approve_plan", {{"project", project}, {"graph", "churn"}});
        ASSERT_TRUE(approved.ok) << approved.error.message;
    }
};

TEST_F(GettingStartedChurn, CsvTrainsEndToEndAndStudyTrialUsesTheSameFeatures) {
    upload("accounts", getting_started_csv());

    // analyze_dataset reports only the named key as a candidate key.
    const ApiResult analysis =
        call("analyze_dataset", {{"database", "getting_started"}, {"dataset", "accounts"}});
    ASSERT_TRUE(analysis.ok) << analysis.error.message;
    ASSERT_EQ(analysis.value.at("candidate_keys").size(), 1u) << analysis.value.dump();
    EXPECT_EQ(analysis.value.at("candidate_keys")[0].at("columns"), Json::array({"account_id"}));

    build_pipeline("getting started");
    const ApiResult started = call("run", {{"project", "getting started"},
                                           {"graph", "churn"},
                                           {"dataset_path", "getting_started.accounts"}});
    ASSERT_TRUE(started.ok) << started.error.message;
    const std::string run_id = started.value.at("run_id");
    ASSERT_TRUE(pump_until([&] {
        const auto run = node.find_run(run_id);
        return run && (run->status() == zetta::pipeline::RunStatus::Succeeded ||
                       run->status() == zetta::pipeline::RunStatus::Failed);
    }));
    const auto run = node.find_run(run_id);
    ASSERT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);

    const ApiResult report = call("get_run_report", {{"run_id", run_id}});
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_NE(report.value.dump().find("held_out_auc"), std::string::npos)
        << report.value.dump();
    const Json& features = report.value.at("features");
    const std::set<std::string> run_used = used_features(features);
    EXPECT_EQ(run_used, std::set<std::string>(std::begin(kMeasures), std::end(kMeasures)))
        << features.dump();
    const Json* plan = excluded(features, "plan");
    ASSERT_NE(plan, nullptr) << features.dump();
    EXPECT_NE(plan->at("why").get<std::string>().find("0/1 column per level"), std::string::npos)
        << plan->dump();
    EXPECT_NE(excluded(features, "account_id"), nullptr);
    EXPECT_NE(excluded(features, "churned"), nullptr);

    // A Study trial over the same table plans its features the same way.
    const ApiResult created = call(
        "create_study",
        {{"name", "getting started study"},
         {"dataset", "getting_started.accounts"},
         {"methodology", "M-1"},
         {"objective",
          {{"metric", "validation.auc"}, {"direction", "maximize"},
           {"validation_split", "validation"}}},
         {"search_space", Json::array({integer_parameter("gbdt.num_trees", 8, 9),
                                       integer_parameter("gbdt.max_depth", 2, 3)})},
         {"sampler", "grid"},
         {"pruner", "none"},
         {"budgets",
          {{"max_trials", 1}, {"max_wall_time_ms", 0}, {"max_parallel_trials", 1},
           {"cost_cap", 0.0}}},
         {"seed", 1216}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string study_id = created.value.at("id");
    ASSERT_TRUE(pump_until([&] {
        const auto study = node.find_study(study_id);
        return study && study->status() == zetta::ml::StudyStatus::Complete;
    })) << call("get_study", {{"study", study_id}}).value.dump();
    const auto study = node.find_study(study_id);
    ASSERT_EQ(study->value().trials.size(), 1u);
    const zetta::ml::StudyTrial& trial = study->value().trials.front();
    EXPECT_EQ(trial.state, zetta::ml::TrialState::Complete) << trial.reason;
    const auto trial_run = node.find_run(trial.run);
    ASSERT_TRUE(trial_run);
    EXPECT_EQ(used_features(trial_run->value().features), run_used)
        << trial_run->value().features.dump();
    EXPECT_NE(excluded(trial_run->value().features, "plan"), nullptr);
    const auto model = node.find_model(trial.model);
    ASSERT_TRUE(model);
    EXPECT_EQ(model->feature_schema().value("features", Json::array()).size(), run_used.size())
        << model->feature_schema().dump();
}

TEST_F(GettingStartedChurn, NoInputsFailureNamesExcludedColumnsAndHowToIncludeThem) {
    std::string csv = "account_id,plan,region,churned\n";
    for (int index = 0; index < 120; ++index) {
        csv += std::format("{},{},{},{}\n", 100 + index, index % 3 == 0 ? "basic" : "pro",
                           index % 2 == 0 ? "north" : "south", index % 4 == 0 ? 1 : 0);
    }
    upload("text_only", csv);
    build_pipeline("text only");
    const ApiResult refused = call("run", {{"project", "text only"},
                                           {"graph", "churn"},
                                           {"dataset_path", "getting_started.text_only"}});
    ASSERT_FALSE(refused.ok);
    const std::string& message = refused.error.message;
    EXPECT_NE(message.find("create_eligible_features"), std::string::npos) << message;
    EXPECT_NE(message.find("Excluded columns: "), std::string::npos) << message;
    for (const auto* column : {"plan", "region"}) {
        EXPECT_NE(message.find(std::string(column) + " — text categories"), std::string::npos)
            << message;
    }
    EXPECT_NE(message.find("0/1 column per level"), std::string::npos) << message;
    EXPECT_TRUE(node.runs().empty());
}

} // namespace
