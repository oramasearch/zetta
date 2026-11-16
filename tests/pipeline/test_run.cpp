// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <latch>
#include <limits>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/transformer.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/node.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::pipeline::Run;
using zetta::pipeline::RunLineage;
using zetta::pipeline::RunLogEntry;
using zetta::pipeline::RunOutput;
using zetta::pipeline::RunStatus;
using zetta::pipeline::RunValue;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A sample run value with every part populated (status / logs / outputs /
// lineage), used to assert it round-trips through disk + the CRDT.
RunValue sample_value() {
    RunValue value;
    value.status = RunStatus::Succeeded;
    value.outputs = {{"dataset", "churn_features", "tbl:abc"},
                     {"index", "churn_index", "idx:def"}};
    value.lineage.sources = {"events.jsonl", "crm.csv"};
    value.lineage.steps = {"connect", "clean", "model", "index"};
    value.lineage.freshness = "2026-06-25T00:00:00.000Z";
    value.lineage.confidence = 0.87;
    return value;
}

// Tick both nodes until `done` holds (or we give up). Mirrors the project /
// data replication tests: capture -> cluster gossip -> peer merge -> reconcile.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

}  // namespace

TEST(Run, CreateUpdateQuery) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // Create: starts Pending with create == update, holds the references.
    auto run = node.create_run("project-uuid", "graph-1");
    ASSERT_TRUE(run);
    EXPECT_EQ(run->project(), "project-uuid");
    EXPECT_EQ(run->graph(), "graph-1");
    EXPECT_EQ(run->status(), RunStatus::Pending);
    EXPECT_FALSE(run->created().empty());
    EXPECT_EQ(run->created(), run->updated());
    EXPECT_TRUE(run->logs().empty());
    EXPECT_TRUE(run->outputs().empty());

    // Update: status, a log line, outputs, and lineage.
    run->set_status(RunStatus::Running);
    run->add_log({"2026-06-25T14:00:00.000Z", "info", "run started"});
    run->set_status(RunStatus::Succeeded);
    run->add_log({"2026-06-25T14:05:00.000Z", "info", "run succeeded"});
    run->set_outputs({{"model", "churn_model", "lo:xyz"}});
    RunLineage lineage;
    lineage.sources = {"events.jsonl"};
    lineage.steps = {"connect", "model"};
    lineage.confidence = 0.5;
    run->set_lineage(lineage);

    EXPECT_GE(run->updated(), run->created());  // bumped by the updates

    // Query: by id; runs are keyed by id (no human name).
    auto found = node.find_run(run->id());
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id(), run->id());
    EXPECT_EQ(found->status(), RunStatus::Succeeded);
    ASSERT_EQ(found->logs().size(), 2u);
    EXPECT_EQ(found->logs()[1].message, "run succeeded");
    ASSERT_EQ(found->outputs().size(), 1u);
    EXPECT_EQ(found->outputs()[0].kind, "model");
    EXPECT_EQ(found->lineage().steps.size(), 2u);
    EXPECT_DOUBLE_EQ(found->lineage().confidence, 0.5);

    EXPECT_EQ(node.run_count(), 1u);
    EXPECT_FALSE(node.find_run("nope"));

    EXPECT_TRUE(node.drop_run(run->id()));
    EXPECT_EQ(node.run_count(), 0u);
    EXPECT_FALSE(node.drop_run(run->id()));  // already gone

    node.stop();
}

TEST(Run, ConfidenceIsClampedTo01) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto run = node.create_run();
    RunLineage hot;
    hot.confidence = 1.5;  // out of range
    run->set_lineage(hot);
    EXPECT_DOUBLE_EQ(run->lineage().confidence, 1.0);

    RunLineage cold;
    cold.confidence = -0.3;
    run->set_lineage(cold);
    EXPECT_DOUBLE_EQ(run->lineage().confidence, 0.0);

    node.stop();
}

TEST(RunFeaturePlan, FeatureReasonsSurviveRestartAndRejectMalformedUsage) {
    TempDir directory;
    std::string run_id;
    const nlohmann::json usage = {
        {"used", {{{"name", "spend"}, {"source", {"spend"}},
                    {"transform", "numeric carry-through"}, {"why", "pre-outcome input"}}}},
        {"excluded", {{{"name", "label"}, {"why", "target only"}},
                       {{"name", "entity"}, {"why", "row key only"}}}}};
    {
        Node node{config_for(directory)};
        ASSERT_TRUE(node.start());
        const auto run = node.create_run();
        ASSERT_TRUE(run);
        run_id = run->id();
        std::string error;
        ASSERT_TRUE(run->set_features(usage, &error)) << error;
        auto malformed = usage;
        malformed["used"][0].erase("why");
        EXPECT_FALSE(run->set_features(malformed, &error));
        EXPECT_EQ(run->value().features, usage);
        node.stop();
    }
    Node restored{config_for(directory)};
    ASSERT_TRUE(restored.start());
    ASSERT_TRUE(restored.find_run(run_id));
    EXPECT_EQ(restored.find_run(run_id)->value().features, usage);
    restored.stop();
}

TEST(RunMetrics, ParamsSummaryArtifactsAndDownsamplingAreDurable) {
    TempDir directory;
    std::string run_id;
    {
        Node node{config_for(directory)};
        ASSERT_TRUE(node.start());
        const auto run = node.create_run("project", "trainer");
        ASSERT_TRUE(run);
        run_id = run->id();

        std::string error;
        ASSERT_TRUE(run->log_params(
            {{"optimizer.learning_rate", 0.01},
             {"model.layers", 2},
             {"device", "cpu"}},
            &error))
            << error;
        EXPECT_FALSE(run->log_params({{"duplicate", true}}, &error));

        for (std::uint64_t step = 0; step < 100; ++step) {
            double loss = 100.0 - static_cast<double>(step);
            if (step == 37) {
                loss = 1'000.0;
            }
            if (step == 68) {
                loss = -1'000.0;
            }
            ASSERT_TRUE(run->log_metrics(
                step, {{"train.loss", loss}, {"train.tokens_per_second", 5.0 + step}},
                "2026-09-28T00:00:00.000Z", &error))
                << error;
        }
        EXPECT_FALSE(run->log_metrics(99, {{"train.loss", 0.0}}, {}, &error));

        ASSERT_TRUE(run->log_artifact(
            {{"type", "feature_importance"},
             {"name", "gain"},
             {"data", {{"features", {"age", "spend"}},
                       {"values", {0.25, 0.75}}}}},
            &error))
            << error;
        EXPECT_FALSE(run->log_artifact(
            {{"type", "feature_importance"},
             {"name", "bad"},
             {"data", {{"features", {"age"}},
                       {"values", {1.0}},
                       {"unexpected", true}}}},
            &error));

        const nlohmann::json read = run->get_metrics(
            {"train.loss"}, /*from_step=*/0, /*max_points=*/8);
        ASSERT_EQ(read.at("series").size(), 1u);
        const nlohmann::json& points = read["series"][0]["points"];
        ASSERT_EQ(points.size(), 8u);
        bool saw_minimum = false;
        bool saw_maximum = false;
        for (const nlohmann::json& point : points) {
            saw_minimum = saw_minimum || point.at("value") == -1'000.0;
            saw_maximum = saw_maximum || point.at("value") == 1'000.0;
        }
        EXPECT_TRUE(saw_minimum);
        EXPECT_TRUE(saw_maximum);
        EXPECT_EQ(read["summary"]["train.loss"]["final_step"], 99);
        EXPECT_EQ(read["summary"]["train.loss"]["best_step"], 68);
        EXPECT_EQ(read["summary"]["train.loss"]["direction"], "min");
        node.stop();
    }

    Node restored{config_for(directory)};
    ASSERT_TRUE(restored.start());
    const auto run = restored.find_run(run_id);
    ASSERT_TRUE(run);
    const nlohmann::json read = run->get_metrics({"train.loss"});
    EXPECT_EQ(read["params"]["optimizer.learning_rate"], 0.01);
    EXPECT_EQ(read["series"][0]["points"].size(), 100u);
    EXPECT_EQ(read["artifacts"].size(), 1u);
    restored.stop();
}

TEST(RunMetrics, NodeLoggingPublishesAnExposedLiveTopic) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    const auto run = node.create_run();
    ASSERT_TRUE(run);
    std::string error;
    ASSERT_TRUE(node.log_run_metrics(
        run->id(), 0, {{"train.loss", 1.5}, {"train.rows_per_second", 42.0}},
        "2026-09-28T00:00:00.000Z", &error))
        << error;
    const zetta::pubsub::Address topic{"topic", "run-metrics." + run->id()};
    const auto settings = node.pubsub()->topic_settings(topic);
    ASSERT_TRUE(settings);
    EXPECT_TRUE(settings->exposed);
    const auto stats = node.pubsub()->topic_stats();
    const auto found = std::ranges::find_if(stats, [&](const auto& entry) {
        return entry.address == topic.to_string();
    });
    ASSERT_NE(found, stats.end());
    EXPECT_EQ(found->held_events, 1u);
    node.stop();
}

TEST(RunMetrics, PrivacySuppressionSurvivesRestartAndDoesNotAddSystemMetrics) {
    TempDir directory;
    std::string run_id;
    {
        Node node{config_for(directory)};
        ASSERT_TRUE(node.start());
        const auto run = node.create_run();
        ASSERT_TRUE(run);
        run_id = run->id();
        run->suppress_metrics_for_privacy();
        ASSERT_TRUE(node.log_run_metrics(run_id, 8, {{"epsilon_spent", 4.0}}));
        EXPECT_EQ(run->get_metrics().at("series").size(), 1u);
        auto invalid = run->metadata();
        invalid["metrics_suppressed"] = "unknown";
        EXPECT_FALSE(zetta::pipeline::parse_run_value(invalid));
        node.stop();
    }
    Node restored{config_for(directory)};
    ASSERT_TRUE(restored.start());
    const auto run = restored.find_run(run_id);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->metadata().at("metrics_suppressed"), "differential_privacy");
    EXPECT_EQ(run->get_metrics().at("metrics_suppressed"), "differential_privacy");
    EXPECT_EQ(run->get_metrics().at("series").size(), 1u);
    restored.stop();
}

TEST(RunMetrics, AutomaticSystemSamplesAdvanceWithinTheSameNaturalStep) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    const auto run = node.create_run();
    ASSERT_TRUE(run);
    std::string error;
    ASSERT_TRUE(node.log_run_metrics(
        run->id(), 0, {{"fit.started", 1.0}}, {}, &error))
        << error;
    ASSERT_TRUE(node.log_run_metrics(
        run->id(), 0, {{"train.loss", 0.5}}, {}, &error))
        << error;
    const auto& metrics = run->value().metrics;
    EXPECT_EQ(metrics["series"]["system.memory.used_bytes"]["steps"],
              nlohmann::json::array({0, 1}));
    EXPECT_EQ(metrics["series"]["system.memory.percent"]["steps"],
              nlohmann::json::array({0, 1}));
    EXPECT_EQ(metrics["series"]["train.loss"]["steps"],
              nlohmann::json::array({0}));
    node.stop();
}

TEST(RunMetrics, InitialMetricAndSamplerRaceThroughTheOwnerQueue) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    node.tick();
    const auto run = node.create_run();
    ASSERT_TRUE(run);
    std::latch start{1};
    std::latch queued{2};
    node.set_owner_wakeup([&] {
        queued.count_down();
    });
    bool initial_ok = false;
    bool sampler_ok = false;
    std::string initial_error;
    std::string sampler_error;
    std::jthread initial([&] {
        start.wait();
        node.run_on_owner([&] {
            EXPECT_TRUE(node.on_owner_thread());
            initial_ok = node.log_run_metrics(
                run->id(), 0, {{"fit.started", 1.0}}, {}, &initial_error);
        });
    });
    std::jthread sampler([&] {
        start.wait();
        node.run_on_owner([&] {
            EXPECT_TRUE(node.on_owner_thread());
            sampler_ok = node.log_run_metrics(
                run->id(), 0, {{"train.loss", 0.5}}, {}, &sampler_error);
        });
    });
    start.count_down();
    // Both producers must be queued before the owner allocates either step.
    queued.wait();
    node.set_owner_wakeup({});
    node.drain_owner_work();
    initial.join();
    sampler.join();
    EXPECT_TRUE(initial_ok) << initial_error;
    EXPECT_TRUE(sampler_ok) << sampler_error;
    const auto& series = run->value().metrics["series"];
    EXPECT_EQ(series["system.memory.used_bytes"]["steps"],
              nlohmann::json::array({0, 1}));
    EXPECT_EQ(series["system.memory.percent"]["steps"],
              nlohmann::json::array({0, 1}));
    node.stop();
}

TEST(RunMetrics, AutomaticStepsArePerRunAndMetricAndSurviveRestart) {
    TempDir directory;
    std::string run_id;
    {
        Node node{config_for(directory)};
        ASSERT_TRUE(node.start());
        const auto run = node.create_run();
        ASSERT_TRUE(run);
        run_id = run->id();
        ASSERT_TRUE(run->log_metrics(40, {{"system.memory.percent", 10.0}}));
        ASSERT_TRUE(node.log_run_metrics(run_id, 4, {{"fit.started", 1.0}}));
        ASSERT_TRUE(node.log_run_metrics(run_id, 4, {{"train.loss", 0.5}}));
        const auto& series = run->value().metrics["series"];
        EXPECT_EQ(series["system.memory.percent"]["steps"],
                  nlohmann::json::array({40, 41, 42}));
        EXPECT_EQ(series["system.memory.used_bytes"]["steps"],
                  nlohmann::json::array({4, 5}));

        const auto other_run = node.create_run();
        ASSERT_TRUE(other_run);
        ASSERT_TRUE(node.log_run_metrics(
            other_run->id(), 4, {{"fit.started", 1.0}}));
        EXPECT_EQ(other_run->value().metrics["series"]
                      ["system.memory.percent"]["steps"],
                  nlohmann::json::array({4}));
        node.stop();
    }
    Node restored{config_for(directory)};
    ASSERT_TRUE(restored.start());
    const auto run = restored.find_run(run_id);
    ASSERT_TRUE(run);
    ASSERT_TRUE(restored.log_run_metrics(run_id, 1, {{"validation.auc", 0.8}}));
    ASSERT_TRUE(restored.log_run_metrics(run_id, 100, {{"train.loss", 0.4}}));
    const auto& metrics = run->value().metrics;
    EXPECT_EQ(metrics["series"]["system.memory.percent"]["steps"],
              nlohmann::json::array({40, 41, 42, 43, 100}));
    EXPECT_EQ(metrics["series"]["system.memory.used_bytes"]["steps"],
              nlohmann::json::array({4, 5, 6, 100}));
    EXPECT_EQ(metrics["summary"]["system.memory.percent"]["final_step"], 100);
    restored.stop();
}

TEST(RunMetrics, CallerStepsRemainStrictAndRejectionIsAtomic) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    const auto run = node.create_run();
    ASSERT_TRUE(run);
    ASSERT_TRUE(node.log_run_metrics(
        run->id(), 3, {{"train.loss", 0.5}, {"system.memory.percent", 25.0}}));
    const auto before = run->value().metrics;
    EXPECT_EQ(before["series"]["system.memory.percent"]["values"],
              nlohmann::json::array({25.0}));
    std::string error;
    EXPECT_FALSE(node.log_run_metrics(
        run->id(), 3, {{"train.loss", 0.4}}, {}, &error));
    EXPECT_NE(error.find("train.loss"), std::string::npos);
    EXPECT_EQ(run->value().metrics, before);
    EXPECT_FALSE(node.log_run_metrics(
        run->id(), 2, {{"train.loss", 0.4}}, {}, &error));
    EXPECT_EQ(run->value().metrics, before);
    EXPECT_FALSE(node.log_run_metrics(
        run->id(), 3, {{"system.memory.percent", 26.0}}, {}, &error));
    EXPECT_NE(error.find("system.memory.percent"), std::string::npos);
    EXPECT_EQ(run->value().metrics, before);
    node.stop();
}

TEST(RunMetrics, AutomaticStepExhaustionDoesNotWrapOrPartiallyCommit) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    const auto run = node.create_run();
    ASSERT_TRUE(run);
    std::string error;
    // The JSON Schema validator accepts signed 64-bit nonnegative integers.
    ASSERT_TRUE(run->log_metrics(std::numeric_limits<std::int64_t>::max(),
                                 {{"system.memory.used_bytes", 1.0}}, {}, &error))
        << error;
    const auto before = run->value().metrics;
    EXPECT_FALSE(node.log_run_metrics(
        run->id(), 0, {{"train.loss", 0.4}}, {}, &error));
    EXPECT_NE(error.find("system.memory.used_bytes"), std::string::npos);
    EXPECT_EQ(run->value().metrics, before);
    node.stop();
}

TEST(RunMetrics, AutomaticLiveBatchesMatchStoredSteps) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    const auto run = node.create_run();
    ASSERT_TRUE(run);
    const std::string time = "2026-09-30T00:00:00.000Z";
    ASSERT_TRUE(node.log_run_metrics(run->id(), 7, {{"fit.started", 1.0}}, time));
    ASSERT_TRUE(node.log_run_metrics(run->id(), 7, {{"train.loss", 0.5}}, time));
    const zetta::pubsub::Identity engine{
        zetta::pubsub::Address{"service", "run-metrics"}, {}};
    zetta::pubsub::SubscribeOptions options;
    options.start = zetta::pubsub::StartPosition::Oldest;
    auto subscription = node.pubsub()->subscribe(
        engine, {"agent", "metric-test"}, {"topic", "run-metrics." + run->id()},
        options);
    ASSERT_TRUE(subscription);
    nlohmann::json observed = nlohmann::json::object();
    for (int batch_index = 0; batch_index < 3; ++batch_index) {
        const auto delivery = subscription->receive(std::chrono::seconds(5));
        ASSERT_TRUE(delivery);
        const auto batch = nlohmann::json::parse(delivery->event.payload.bytes());
        EXPECT_EQ(batch.at("run"), run->id());
        EXPECT_EQ(batch.at("time"), time);
        for (const auto& [name, value] : batch.at("values").items()) {
            observed[name]["steps"].push_back(batch.at("step"));
            observed[name]["values"].push_back(value);
            observed[name]["times"].push_back(batch.at("time"));
        }
    }
    EXPECT_EQ(observed, run->value().metrics.at("series"));
    EXPECT_FALSE(subscription->receive(std::chrono::milliseconds(0)));
    subscription.reset();
    node.stop();
}

TEST(RunMetrics, QwenFineTuneProducesReadableAndLiveLossCurves) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    const auto run = node.create_run("qwen-project", "fine-tune");
    ASSERT_TRUE(run);
    std::string error;
    ASSERT_TRUE(node.log_run_params(
        run->id(),
        {{"trainer", "qwen"},
         {"optimizer.learning_rate", 0.01},
         {"model.layers", 1}},
        &error))
        << error;

    zetta::autograd::QwenConfig config;
    config.vocab_size = 16;
    config.d_model = 8;
    config.n_layers = 1;
    config.n_heads = 2;
    config.n_kv_heads = 1;
    config.head_dim = 4;
    config.d_ff = 16;
    zetta::autograd::QwenModel model(config, 7);
    zetta::autograd::Adam optimizer(0.01F);
    const std::vector<int> tokens{2, 5, 7, 3, 9, 1};
    const auto logger = [&](std::uint64_t step,
                            const std::map<std::string, double>& values) {
        return node.log_run_metrics(run->id(), step, values, {}, &error);
    };
    for (std::uint64_t step = 0; step < 4; ++step) {
        const auto metrics = zetta::autograd::fine_tune_qwen_step(
            model, optimizer, tokens, 0, step, logger, &error);
        ASSERT_TRUE(metrics) << error;
        EXPECT_TRUE(std::isfinite(metrics->gradient_norm));
        EXPECT_GT(metrics->tokens_per_second, 0.0);
    }

    const nlohmann::json read = run->get_metrics({"train.loss"});
    ASSERT_EQ(read["series"].size(), 1u);
    EXPECT_EQ(read["series"][0]["points"].size(), 4u);
    EXPECT_EQ(read["params"]["trainer"], "qwen");
    EXPECT_TRUE(read["summary"].contains("train.loss"));
    const zetta::pubsub::Address topic{"topic", "run-metrics." + run->id()};
    const auto topic_stats = node.pubsub()->topic_stats();
    const auto found = std::ranges::find_if(
        topic_stats, [&](const auto& entry) {
            return entry.address == topic.to_string();
        });
    ASSERT_NE(found, topic_stats.end());
    EXPECT_EQ(found->held_events, 4u);
    node.stop();
}

TEST(Run, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto run = node.create_run("project-uuid", "graph-1");
        ASSERT_TRUE(run);
        id = run->id();
        run->apply_value([] {
            RunValue value = sample_value();
            value.project = "project-uuid";
            value.graph = "graph-1";
            return value;
        }());
        node.stop();
    }

    // A fresh standalone node reads the run purely from disk (run.json).
    Node reloaded{config_for(tmp)};
    reloaded.start();
    EXPECT_EQ(reloaded.run_count(), 1u);
    auto run = reloaded.find_run(id);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), RunStatus::Succeeded);
    EXPECT_EQ(run->project(), "project-uuid");
    ASSERT_EQ(run->outputs().size(), 2u);
    EXPECT_EQ(run->outputs()[0].name, "churn_features");
    EXPECT_EQ(run->lineage().sources.size(), 2u);
    EXPECT_DOUBLE_EQ(run->lineage().confidence, 0.87);
    reloaded.stop();
}

TEST(Run, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::util::new_uuid();
    std::string id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto run = node.create_run("project-uuid", "graph-1");
        ASSERT_TRUE(run);
        id = run->id();
        run->set_status(RunStatus::Succeeded);
        run->set_outputs({{"index", "churn_index", "idx:def"}});
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document: the run
    // is in the replicated document, and the projection is rebuilt from it.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int run_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("run:", 0) == 0) {
            ++run_keys;
            EXPECT_EQ(value.value("status", std::string{}), "succeeded");
        }
    }
    EXPECT_EQ(run_keys, 1);

    auto run = node.find_run(id);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), RunStatus::Succeeded);
    ASSERT_EQ(run->outputs().size(), 1u);
    EXPECT_EQ(run->outputs()[0].kind, "index");
    node.stop();
}

TEST(Run, ReplicatesToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43320;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43321;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43320"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Create + complete a run on node A only.
    auto created = a.create_run("project-uuid", "graph-1");
    ASSERT_TRUE(created);
    created->set_status(RunStatus::Succeeded);
    created->set_outputs({{"dataset", "churn_features", "tbl:abc"}});

    // Node B converges to the same run (status + outputs), versions match.
    const bool converged = converge(a, b, [&] {
        auto run = b.find_run(created->id());
        return run && run->status() == RunStatus::Succeeded &&
               run->outputs().size() == 1u &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    auto replicated = b.find_run(created->id());
    ASSERT_TRUE(replicated);
    EXPECT_EQ(replicated->project(), "project-uuid");
    EXPECT_EQ(replicated->outputs()[0].name, "churn_features");

    // A further update on A also replicates to B.
    created->set_status(RunStatus::Failed);
    const bool updated = converge(a, b, [&] {
        auto run = b.find_run(created->id());
        return run && run->status() == RunStatus::Failed;
    });
    EXPECT_TRUE(updated);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}

TEST(Run, SchemaRejectsMalformedValuesAtTheBoundary) {
    using nlohmann::json;
    std::string error;

    // A well-formed value passes.
    EXPECT_TRUE(zetta::pipeline::validate_run_value(zetta::pipeline::to_json(sample_value()), &error))
        << error;

    // Unknown status enum.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "exploded"}}, &error));
    // Missing the required status field.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(json{{"project", "p"}}, &error));
    // Wrong type for a field.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "running"}, {"logs", "not-an-array"}}, &error));
    // Confidence out of range.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "running"}, {"lineage", {{"confidence", 2.0}}}},
        &error));
    // An unknown output kind.
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "running"},
             {"outputs", json::array({{{"kind", "spreadsheet"}}})}},
        &error));
    // A stray top-level field (additionalProperties: false).
    EXPECT_FALSE(zetta::pipeline::validate_run_value(
        json{{"status", "running"}, {"surprise", 1}}, &error));

    // parse_run_value rejects the same and returns nullopt with a reason.
    EXPECT_FALSE(zetta::pipeline::parse_run_value(json{{"status", "exploded"}}, &error));
    EXPECT_FALSE(error.empty());
}

TEST(Run, InvalidRunJsonOnDiskIsSkipped) {
    TempDir tmp;
    // Plant a folder with a malformed run.json (bad status) before start().
    const std::filesystem::path bad = tmp.path() / zetta::util::new_uuid();
    std::filesystem::create_directories(bad);
    {
        std::ofstream out(bad / "run.json");
        out << R"({"id":"x","status":"exploded"})" << '\n';
    }

    Node node{config_for(tmp)};
    node.start();
    // The malformed run is skipped at the boundary, not loaded.
    EXPECT_EQ(node.run_count(), 0u);
    node.stop();
}
