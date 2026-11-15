// SPDX-License-Identifier: MIT

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
#include "zetta/ml/model.hpp"
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
    // #1203: the text column `plan` is one-hot encoded, one feature per level.
    std::set<std::string> expected_used(std::begin(kMeasures), std::end(kMeasures));
    expected_used.insert({"plan=basic", "plan=enterprise", "plan=pro"});
    EXPECT_EQ(run_used, expected_used) << features.dump();
    EXPECT_EQ(excluded(features, "plan"), nullptr) << features.dump();
    for (const Json& used : features.at("used")) {
        if (used.at("source") != Json::array({"plan"})) continue;
        EXPECT_NE(used.at("transform").get<std::string>().find("one-hot"), std::string::npos)
            << used.dump();
    }
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
    EXPECT_EQ(excluded(trial_run->value().features, "plan"), nullptr);
    const auto model = node.find_model(trial.model);
    ASSERT_TRUE(model);
    EXPECT_EQ(model->feature_schema().value("features", Json::array()).size(), run_used.size())
        << model->feature_schema().dump();
}

TEST_F(GettingStartedChurn, NoInputsFailureNamesExcludedColumnsAndHowToIncludeThem) {
    // Text columns that are not categories: a unique email per row (an
    // identifier) and a city with 40 levels (above the 32-level one-hot cap).
    std::string csv = "account_id,email,city,churned\n";
    for (int index = 0; index < 120; ++index) {
        csv += std::format("{},user{}@example.com,city_{},{}\n", 100 + index, index,
                           index % 40, index % 4 == 0 ? 1 : 0);
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
    EXPECT_NE(message.find("email — identifiers are excluded"), std::string::npos) << message;
    EXPECT_NE(message.find("city — text with 40 distinct values is above the 32-level cap"),
              std::string::npos)
        << message;
    EXPECT_TRUE(node.runs().empty());
}

// #1203: a churn table whose signal is mostly in text columns. `region` decides most
// of the label and `plan` some of it; `usage` is the only number and carries little.
std::string categorical_signal_csv() {
    constexpr std::array<std::string_view, 4> regions = {"north", "south", "east", "west"};
    std::string csv = "account_id,usage,region,plan,contact_email,churned\n";
    for (int index = 0; index < 600; ++index) {
        const int usage = (index * 37) % 100;
        const std::string_view region = regions[(index * 7 / 3) % 4];
        const std::string_view plan = (index / 5) % 2 == 0 ? "basic" : "pro";
        int churned = (region == "south" || (plan == "basic" && region == "east")) ? 1 : 0;
        if (index % 13 == 0) churned = 1 - churned;
        csv += std::format("{},{},{},{},owner{}@example.com,{}\n", 9000 + index, usage, region,
                           plan, index, churned);
    }
    return csv;
}

TEST_F(GettingStartedChurn, TextCategoriesAreEncodedAndImproveTheHeldOutScore) {
    upload("regional", categorical_signal_csv());

    const auto train = [&](const std::string& project, const Json& extra) -> std::string {
        build_pipeline(project);
        Json input = {{"project", project},
                      {"graph", "churn"},
                      {"dataset_path", "getting_started.regional"},
                      {"holdout_fraction", 0.25}};
        input.update(extra);
        const ApiResult started = call("run", input);
        EXPECT_TRUE(started.ok) << started.error.message;
        if (!started.ok) return {};
        const std::string run_id = started.value.at("run_id");
        EXPECT_TRUE(pump_until([&] {
            const auto run = node.find_run(run_id);
            return run && (run->status() == zetta::pipeline::RunStatus::Succeeded ||
                           run->status() == zetta::pipeline::RunStatus::Failed);
        }));
        const auto run = node.find_run(run_id);
        EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);
        return run_id;
    };
    const auto model_of = [&](const std::string& run_id) {
        std::shared_ptr<zetta::ml::Model> found;
        for (const auto& model : node.models()) {
            if (model && model->value().run == run_id) found = model;
        }
        return found;
    };

    // With the categories (inferred) and without them (only `usage` named).
    const std::string with_run = train("with categories", Json::object());
    const std::string without_run =
        train("numbers only", {{"dataset", {{"feature_columns", {"usage"}}}}});
    ASSERT_FALSE(with_run.empty());
    ASSERT_FALSE(without_run.empty());
    const auto with_model = model_of(with_run);
    const auto without_model = model_of(without_run);
    ASSERT_TRUE(with_model);
    ASSERT_TRUE(without_model);

    // The run's report names the categorical columns and how they were encoded;
    // the identifier-like email is still excluded, saying why.
    const auto run = node.find_run(with_run);
    const Json& features = run->value().features;
    const std::set<std::string> used = used_features(features);
    for (const auto* feature : {"usage", "region=east", "region=north", "region=south",
                                "region=west", "plan=basic", "plan=pro"}) {
        EXPECT_TRUE(used.contains(feature)) << feature << " " << features.dump();
    }
    EXPECT_FALSE(used.contains("contact_email")) << features.dump();
    bool logged_region = false;
    bool logged_email = false;
    for (const auto& log : run->logs()) {
        if (log.message.find("categorical feature: 'region' one-hot encoded into region=east, "
                             "region=north, region=south, region=west") != std::string::npos) {
            logged_region = true;
        }
        if (log.message.find("not used: 'contact_email'") != std::string::npos) {
            logged_email = true;
        }
    }
    EXPECT_TRUE(logged_region) << "the run log names the encoded category";
    EXPECT_TRUE(logged_email) << "the run log says why the email column is not used";

    // The encoding is stored with the model, and the input contract names the
    // original columns: numbers for numeric features, text with levels for categories.
    const Json& schema = with_model->feature_schema();
    ASSERT_TRUE(schema.contains("categorical_encoding")) << schema.dump();
    Json region_input;
    for (const Json& input : schema.at("inputs")) {
        if (input.at("name") == "region") region_input = input;
        EXPECT_NE(input.at("name"), "contact_email");
    }
    ASSERT_FALSE(region_input.is_null()) << schema.dump();
    EXPECT_EQ(region_input.at("type"), "string");
    EXPECT_EQ(region_input.at("levels"), Json::array({"east", "north", "south", "west"}));

    // The categories carry the signal: the held-out AUC is clearly higher with them.
    ASSERT_TRUE(with_model->metrics().contains("held_out_auc")) << with_model->metrics().dump();
    ASSERT_TRUE(without_model->metrics().contains("held_out_auc"))
        << without_model->metrics().dump();
    const double with_auc = with_model->metrics().at("held_out_auc").get<double>();
    const double without_auc = without_model->metrics().at("held_out_auc").get<double>();
    RecordProperty("held_out_auc_with_categories", std::to_string(with_auc));
    RecordProperty("held_out_auc_numbers_only", std::to_string(without_auc));
    EXPECT_GT(with_auc, without_auc + 0.15)
        << "with categories " << with_auc << ", numbers only " << without_auc;

    // Predict by original names and text values: a seen category, an unseen one
    // (all zeros, not an error), and a row without the column (an error).
    const ApiResult scored = call(
        "score", {{"model", with_model->id()},
                  {"rows", Json::array({{{"usage", 50}, {"region", "south"}, {"plan", "pro"}},
                                        {{"usage", 50}, {"region", "north"}, {"plan", "pro"}},
                                        {{"usage", 50}, {"region", "antarctica"},
                                         {"plan", "enterprise"}}})},
                  {"entities", Json::array({1, 2, 3})}});
    ASSERT_TRUE(scored.ok) << scored.error.message;
    ASSERT_EQ(scored.value.at("ranked").size(), 3u);
    double south = -1.0;
    double north = -1.0;
    for (const Json& item : scored.value.at("ranked")) {
        const double risk = item.at("risk").get<double>();
        EXPECT_GE(risk, 0.0);
        EXPECT_LE(risk, 1.0);
        if (item.at("entity") == 1) south = risk;
        if (item.at("entity") == 2) north = risk;
    }
    EXPECT_GT(south, north) << scored.value.dump();

    const ApiResult explained =
        call("explain", {{"model", with_model->id()},
                         {"row", {{"usage", 50}, {"region", "south"}, {"plan", "basic"}}}});
    ASSERT_TRUE(explained.ok) << explained.error.message;

    const ApiResult missing = call(
        "score", {{"model", with_model->id()},
                  {"rows", Json::array({{{"usage", 50}, {"plan", "pro"}}})}});
    ASSERT_FALSE(missing.ok);
    EXPECT_NE(missing.error.message.find("'region'"), std::string::npos)
        << missing.error.message;

    // Scoring the table itself reads the original text columns with the stored levels.
    const ApiResult table_scored =
        call("score", {{"model", with_model->id()},
                       {"dataset_path", "getting_started.regional"}});
    ASSERT_TRUE(table_scored.ok) << table_scored.error.message;
    EXPECT_EQ(table_scored.value.at("rows_scored"), 600);
}

} // namespace
