// #1497 (seen from lake#1243): a clustering model trained with train_model
// (methodology M-3, as Lake's Quickstart trains it) answers `score` and
// `explain`. Before, both read the weights as gradient-boosted trees and
// failed with "required property 'objective' not found".
// docs/ml/segmentation.md#scoring-and-explaining.
#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <iostream>
#include <map>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

namespace {

using Json = nlohmann::json;
using zetta::api::ApiContext;
using zetta::api::ApiResult;

// Two kinds of customer: low spend with many visits on the free plan, high
// spend with few visits on pro.
std::string customers_csv() {
    std::string csv = "customer_id,spend,visits,plan\n";
    for (int index = 0; index < 80; ++index) {
        const bool high = index % 2 == 1;
        const double jitter = (index % 7) * 0.1;
        csv += std::format("{},{:.2f},{:.2f},{}\n", 5000 + index,
                           (high ? 90.0 : 10.0) + jitter, (high ? 2.0 : 20.0) - jitter,
                           high ? "pro" : "free");
    }
    return csv;
}

class SegmentationScoreApi : public ::testing::Test {
protected:
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{"segmentation-score-node"};
    zetta::Node node{node_config(), &replication};
    zetta::cluster::Cluster cluster{"segmentation-score-node", cluster_config(), &replication};
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
        config.listen_port = 24970;
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
                                                           {"table_name", "customers"},
                                                           {"format", "csv"},
                                                           {"content", customers_csv()}});
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

    // Train the clustering as Lake's Quickstart does; the model's record.
    Json train_segments() {
        const ApiResult started = call("train_model", {{"name", "segments"},
                                                       {"methodology", "M-3"},
                                                       {"dataset_path", "crm.customers"},
                                                       {"entity_column", "customer_id"}});
        EXPECT_TRUE(started.ok) << started.error.code << ": " << started.error.message;
        if (!started.ok) return {};
        const std::string run_id = started.value.at("run_id");
        EXPECT_EQ(finish(run_id), zetta::pipeline::RunStatus::Succeeded);
        const ApiResult listed = call("list_models", Json::object());
        EXPECT_TRUE(listed.ok);
        for (const Json& model : listed.value.at("models")) {
            if (model.at("name") == "segments") return model;
        }
        ADD_FAILURE() << "no model named segments: " << listed.value.dump();
        return {};
    }

    // The training run's assignment table: customer id -> segment.
    std::map<std::int64_t, int> assignments_written(const std::string& run_id) {
        std::map<std::int64_t, int> out;
        const auto run = node.find_run(run_id);
        if (!run) return out;
        for (const auto& output : run->outputs()) {
            if (output.name != "segment assignments") continue;
            const auto dot = output.ref.find('.');
            const auto database = node.find_database(output.ref.substr(0, dot));
            const auto table = database ? database->find_table(output.ref.substr(dot + 1)) : nullptr;
            if (!table) return out;
            for (std::size_t index = 0; index < table->row_count(); ++index) {
                const auto row = table->store().at(index);
                if (row) out[row->at("customer_id").get<std::int64_t>()] = row->at("segment_id");
            }
        }
        return out;
    }
};

TEST_F(SegmentationScoreApi, ScoreAssignsRowsToTheirNearestSegment) {
    const Json model = train_segments();
    ASSERT_TRUE(model.is_object());
    EXPECT_EQ(model.at("kind"), "segmentation");

    const ApiResult scored =
        call("score", {{"model", model.at("id")},
                       {"rows", Json::array({{{"spend", 10.5}, {"visits", 19.5}, {"plan", "free"}},
                                             {{"spend", 89.0}, {"visits", 2.5}, {"plan", "pro"}}})}});
    ASSERT_TRUE(scored.ok) << scored.error.code << ": " << scored.error.message;
    std::cout << "score answer: " << scored.value.dump() << "\n";
    EXPECT_EQ(scored.value.at("kind"), "segmentation");
    EXPECT_TRUE(scored.value.at("ranked").empty());
    EXPECT_EQ(scored.value.at("feature_columns"), (Json{"spend", "visits", "plan"}));
    const Json& assignments = scored.value.at("assignments");
    ASSERT_EQ(assignments.size(), 2u);
    EXPECT_EQ(assignments[0].at("entity"), 0);
    EXPECT_EQ(assignments[1].at("entity"), 1);
    EXPECT_NE(assignments[0].at("segment_id"), assignments[1].at("segment_id"));
    EXPECT_TRUE(assignments[0].at("label").is_string()) << assignments.dump();
    EXPECT_TRUE(assignments[0].at("distance").is_number());

    // The same rows answer with the caller's keys.
    const ApiResult keyed = call("score", {{"model", model.at("id")},
                                           {"rows", Json::array({{{"spend", 10.5},
                                                                  {"visits", 19.5},
                                                                  {"plan", "free"}}})},
                                           {"entities", Json::array({77})}});
    ASSERT_TRUE(keyed.ok) << keyed.error.message;
    EXPECT_EQ(keyed.value.at("assignments")[0].at("entity"), 77);

    // A missing column says which, never a schema error.
    const ApiResult missing = call("score", {{"model", model.at("id")},
                                             {"rows", Json::array({{{"spend", 10.5}}})}});
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "invalid_input");
    EXPECT_EQ(missing.error.message,
              "score: row 0 has no 'visits'; the model needs spend, visits, plan");
}

TEST_F(SegmentationScoreApi, ScoringTheTrainingTableMatchesTheAssignmentsTrainingWrote) {
    const Json model = train_segments();
    ASSERT_TRUE(model.is_object());
    const auto written = assignments_written(model.at("run_id"));
    ASSERT_EQ(written.size(), 80u);

    const ApiResult scored =
        call("score", {{"model", model.at("id")}, {"dataset_path", "crm.customers"},
                       {"dataset", {{"entity_column", "customer_id"}}}});
    ASSERT_TRUE(scored.ok) << scored.error.code << ": " << scored.error.message;
    const Json& assignments = scored.value.at("assignments");
    ASSERT_EQ(assignments.size(), 80u);
    for (const Json& assignment : assignments) {
        const auto entity = assignment.at("entity").get<std::int64_t>();
        ASSERT_TRUE(written.contains(entity)) << entity;
        EXPECT_EQ(assignment.at("segment_id").get<int>(), written.at(entity)) << entity;
    }
}

TEST_F(SegmentationScoreApi, ExplainSaysWhichColumnsPlaceTheRow) {
    const Json model = train_segments();
    ASSERT_TRUE(model.is_object());
    const ApiResult explained =
        call("explain", {{"model", model.at("id")},
                         {"row", {{"spend", 11.0}, {"visits", 19.0}, {"plan", "free"}}}});
    ASSERT_TRUE(explained.ok) << explained.error.code << ": " << explained.error.message;
    std::cout << "explain answer: " << explained.value.dump() << "\n";
    EXPECT_EQ(explained.value.at("kind"), "segmentation");
    EXPECT_TRUE(explained.value.at("runner_up").is_object());
    EXPECT_EQ(explained.value.at("contributions").size(), 3u);
    EXPECT_FALSE(explained.value.contains("risk"));

    const ApiResult entity =
        call("explain", {{"model", model.at("id")}, {"dataset_path", "crm.customers"},
                         {"dataset", {{"entity_column", "customer_id"}}}, {"entity", 5001}});
    ASSERT_TRUE(entity.ok) << entity.error.code << ": " << entity.error.message;
    EXPECT_EQ(entity.value.at("entity"), 5001);
    EXPECT_EQ(entity.value.at("segment_id").get<int>(),
              assignments_written(model.at("run_id")).at(5001));
}

}  // namespace
