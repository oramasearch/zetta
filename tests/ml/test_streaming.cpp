// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/streaming.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pubsub/pubsub.hpp"

namespace {

std::vector<zetta::ml::StreamingEvent> learnable_events() {
    std::vector<zetta::ml::StreamingEvent> events;
    for (std::size_t index = 0; index < 600; ++index) {
        const double signal = index % 2 == 0 ? 0.0 : 1.0;
        events.push_back({"event-" + std::to_string(index),
                          static_cast<std::int64_t>(index),
                          static_cast<double>(index),
                          {signal},
                          signal,
                          static_cast<double>(index) + 0.5,
                          index});
    }
    return events;
}

} // namespace

TEST(OnlineStreaming, ModelConvergesDeterministicallyOnAnEventStream) {
    zetta::ml::StreamingParams params;
    params.learning_rate = 0.2;
    params.l2_regularization = 0.0001;
    params.window_duration = 0.25;
    params.metric_window = 100;
    params.persist_every = 25;
    params.seed = 1060;

    std::uint64_t persisted_labels = 0;
    std::size_t persistence_count = 0;
    zetta::ml::StreamingCallbacks callbacks;
    callbacks.on_persist = [&](const zetta::ml::OnlineLogisticModel&,
                               std::uint64_t labels) {
        persisted_labels = labels;
        ++persistence_count;
    };
    const zetta::ml::StreamingTrainResult trained =
        zetta::ml::train_streaming(learnable_events(), {"signal"}, params,
                                   {}, {}, callbacks);
    ASSERT_TRUE(trained.ok()) << trained.error;
    EXPECT_EQ(trained.metrics.events_read, 600u);
    EXPECT_EQ(trained.metrics.events_scored, 600u);
    EXPECT_EQ(trained.metrics.evaluated_labels, 600u);
    EXPECT_GT(trained.metrics.accuracy, 0.95);
    EXPECT_LT(trained.metrics.log_loss, 0.2);
    EXPECT_EQ(trained.model.version, 600u);
    EXPECT_EQ(persisted_labels, 600u);
    EXPECT_GT(persistence_count, 1u);

    const zetta::ml::StreamingTrainResult repeated =
        zetta::ml::train_streaming(learnable_events(), {"signal"}, params);
    ASSERT_TRUE(repeated.ok()) << repeated.error;
    EXPECT_EQ(repeated.model.to_json(), trained.model.to_json());
    EXPECT_EQ(repeated.metrics.to_json(), trained.metrics.to_json());
}

TEST(PrequentialStreaming, ALabelIsNeverUsedBeforeItsEventTime) {
    zetta::ml::StreamingParams params;
    params.allowed_lateness = 0.0;
    const std::vector<zetta::ml::StreamingEvent> events = {
        {"future-label", 1, 10.0, {1.0}, 1.0, 9.0, 0},
        {"valid-label", 2, 11.0, {0.0}, 0.0, 12.0, 1}};

    const zetta::ml::StreamingTrainResult trained =
        zetta::ml::train_streaming(events, {"signal"}, params);
    ASSERT_TRUE(trained.ok()) << trained.error;
    EXPECT_EQ(trained.metrics.early_labels_excluded, 1u);
    EXPECT_EQ(trained.metrics.evaluated_labels, 1u);
    EXPECT_EQ(trained.model.version, 1u);
    ASSERT_EQ(trained.scores.size(), 2u);
    EXPECT_EQ(trained.scores.front().model_version, 0u);
    EXPECT_EQ(trained.scores.back().model_version, 0u)
        << "the valid label arrives after both predictions";
}

TEST(PrequentialStreaming, MetricsUseThePredictionMadeBeforeTraining) {
    zetta::ml::StreamingParams params;
    params.learning_rate = 1.0;
    params.l2_regularization = 0.0;
    params.window_duration = 1.0;
    const std::vector<zetta::ml::StreamingEvent> events = {
        {"first", 1, 1.0, {1.0}, 1.0, 1.0, 0}};

    const zetta::ml::StreamingTrainResult trained =
        zetta::ml::train_streaming(events, {"signal"}, params);
    ASSERT_TRUE(trained.ok()) << trained.error;
    ASSERT_EQ(trained.scores.size(), 1u);
    EXPECT_EQ(trained.scores.front().model_version, 0u);
    EXPECT_NEAR(trained.metrics.log_loss, -std::log(0.5), 0.001);
    EXPECT_EQ(trained.model.version, 1u);
}

TEST(StreamingEventTime, EventsBehindTheAllowedLatenessAreExcluded) {
    zetta::ml::StreamingParams params;
    params.allowed_lateness = 2.0;
    const std::vector<zetta::ml::StreamingEvent> events = {
        {"current", 1, 10.0, {1.0}, {}, {}, 0},
        {"allowed", 1, 8.0, {1.0}, {}, {}, 1},
        {"too-late", 1, 7.9, {1.0}, {}, {}, 2}};

    const zetta::ml::StreamingTrainResult trained =
        zetta::ml::train_streaming(events, {"signal"}, params);
    ASSERT_TRUE(trained.ok()) << trained.error;
    EXPECT_EQ(trained.metrics.events_read, 3u);
    EXPECT_EQ(trained.metrics.events_scored, 2u);
    EXPECT_EQ(trained.metrics.late_events_excluded, 1u);
    EXPECT_DOUBLE_EQ(trained.metrics.watermark, 8.0);
}

TEST(OnlineStreaming, ModelAndParametersRejectMalformedBoundaries) {
    std::string reason;
    EXPECT_FALSE(zetta::ml::StreamingParams::from_json(
        {{"learning_rate", 0.0}}, &reason));
    EXPECT_NE(reason.find("learning_rate"), std::string::npos);

    zetta::ml::OnlineLogisticModel model;
    model.feature_names = {"window_count", "window_sum_signal"};
    model.weights = {0.2, -0.1};
    model.bias = 0.3;
    model.version = 7;
    model.seed = 4;
    const auto restored =
        zetta::ml::OnlineLogisticModel::from_json(model.to_json(), &reason);
    ASSERT_TRUE(restored) << reason;
    EXPECT_EQ(restored->to_json(), model.to_json());
}

TEST(StreamingRunner, PublishesScoresAndWritesAResultTableWithLineage) {
    zetta::test::TempDir directory;
    zetta::NodeConfig config;
    config.data_dir = directory.path().string();
    zetta::Node node(config);
    ASSERT_TRUE(node.start());
    const auto database = node.create_database("stream_input");
    ASSERT_TRUE(database);
    const auto table = database->create_table("events");
    ASSERT_TRUE(table);
    for (std::size_t index = 0; index < 40; ++index) {
        const double signal = index % 2 == 0 ? 0.0 : 1.0;
        table->add_row({{"event_id", "row-" + std::to_string(index)},
                        {"entity_id", static_cast<std::int64_t>(index)},
                        {"event_time", static_cast<double>(index)},
                        {"signal", signal},
                        {"label", signal},
                        {"label_time", static_cast<double>(index) + 0.25}});
    }
    zetta::data::ResolvedDataset dataset;
    dataset.database = "stream_input";
    dataset.table = "events";
    dataset.table_id = table->id();
    dataset.rows = table->row_count();
    dataset.contract.entity_column = "entity_id";
    dataset.contract.label_column = "label";
    dataset.contract.time_column = "event_time";
    dataset.contract.feature_columns = {"signal"};
    dataset.notes = {"not used: free_text"};

    zetta::ml::StreamingParams params;
    params.input_topic = "runner.events";
    params.output_topic = "runner.scores";
    params.window_duration = 0.1;
    params.persist_every = 5;
    const zetta::pubsub::Identity service{{"service", "streaming"}, {}};
    zetta::pubsub::TopicSettings settings;
    settings.exposed = true;
    ASSERT_TRUE(node.pubsub()->create_topic(
        service, {"topic", params.output_topic}, settings));
    zetta::pubsub::SubscribeOptions subscribe_options;
    subscribe_options.start = zetta::pubsub::StartPosition::Next;
    const auto subscription = node.pubsub()->subscribe(
        service, {"service", "runner-test"},
        {"topic", params.output_topic}, subscribe_options);
    ASSERT_TRUE(subscription);

    const auto run = node.create_run("project", "graph");
    ASSERT_TRUE(run);
    run->set_status(zetta::pipeline::RunStatus::Running);
    zetta::ml::StreamingRunOptions options;
    options.project = "project";
    options.graph = "graph";
    options.run_id = run->id();
    options.model_name = "runner_model";
    const zetta::ml::StreamingRunResult result =
        zetta::ml::run_streaming(node, dataset, params, options);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);
    EXPECT_FALSE(result.model_id.empty());
    EXPECT_FALSE(result.scores_table.empty());
    EXPECT_EQ(result.excluded, std::vector<std::string>{"free_text"});
    EXPECT_NE(std::find(run->lineage().sources.begin(),
                        run->lineage().sources.end(),
                        "topic:" + params.input_topic),
              run->lineage().sources.end());

    const auto scores_database = node.find_database("streaming_results");
    ASSERT_TRUE(scores_database);
    const std::string scores_name =
        result.scores_table.substr(result.scores_table.find('.') + 1);
    const auto scores_table = scores_database->find_table(scores_name);
    ASSERT_TRUE(scores_table);
    EXPECT_EQ(scores_table->row_count(), 40u);
    for (std::size_t index = 0; index < 40; ++index) {
        const auto delivery = subscription->receive(std::chrono::seconds(1));
        ASSERT_TRUE(delivery) << "missing published score " << index;
        const nlohmann::json score =
            nlohmann::json::parse(delivery->event.payload.bytes());
        EXPECT_TRUE(score.contains("score"));
        EXPECT_TRUE(score.contains("model_version"));
    }
    node.stop();
}
