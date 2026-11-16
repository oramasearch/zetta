// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/ml/forecast_methodology.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using namespace std::chrono_literals;

struct ForecastMember {
  ForecastMember(std::uint16_t port, std::vector<std::string> seeds)
      : replication(id), node({directory.path().string(), false}, &replication),
        blobs(directory.path(), replication),
        cluster(id, config(port, std::move(seeds)), &replication, &blobs) {
    EXPECT_TRUE(node.start());
    cluster.start();
    replication.set("node:" + id,
                    nlohmann::json{{"address", cluster.listen_address()}});
    node.attach_cluster(cluster, 2);
  }

  ~ForecastMember() {
    node.stop();
  }

  static zetta::cluster::ClusterConfig config(std::uint16_t port,
                                              std::vector<std::string> seeds) {
    zetta::cluster::ClusterConfig result;
    result.listen_host = "127.0.0.1";
    result.listen_port = port;
    result.port_range = 4;
    result.seeds = std::move(seeds);
    return result;
  }

  zetta::test::TempDir directory;
  std::string id = zetta::util::new_uuid();
  zetta::cluster::ReplicationManager replication;
  zetta::Node node;
  zetta::data::ReplicatedBlobStore blobs;
  zetta::cluster::Cluster cluster;
};

template <class Predicate> bool patiently(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

zetta::ml::ForecastSeries history() {
  zetta::ml::ForecastSeries result;
  for (std::size_t day = 0; day < 168; ++day) {
    result.times.push_back(static_cast<double>(day));
    result.values.push_back(100.0 + day * 0.5 +
                            10 * std::sin(day * 6.283185307179586 / 7));
  }
  return result;
}

zetta::api::ApiResult call(zetta::api::Registry &registry, zetta::Node &node,
                           zetta::api::BackgroundActions &background,
                           const std::string &method,
                           const nlohmann::json &input) {
  zetta::api::ApiContext context{node,
                                 nullptr,
                                 {},
                                 zetta::observability::make_root_trace(),
                                 &node.span_recorder(),
                                 &background};
  return registry.call(method, input, context);
}

} // namespace

TEST(ForecastLaunch,
     NonRootSubmissionMatchesOracleAndResolvesReplicatedOutputs) {
  ForecastMember first(47620, {});
  ForecastMember second(47624, {first.cluster.listen_address()});
  ASSERT_TRUE(patiently([&] {
    first.node.tick();
    second.node.tick();
    return first.cluster.peer_count() == 1 && second.cluster.peer_count() == 1;
  }));
  ForecastMember &initiator = first.id > second.id ? first : second;
  ForecastMember &root = first.id < second.id ? first : second;
  const auto series = history();
  std::vector<nlohmann::json> rows;
  for (std::size_t index = 0; index < series.times.size(); ++index) {
    rows.push_back(
        {{"timestamp", series.times[index]}, {"value", series.values[index]}});
  }
  zetta::data::AddDatasetOptions add;
  add.database = "datasets";
  add.table_name = "history";
  add.partition_count = 2;
  add.replication_factor = 1;
  add.partition_only = true;
  add.members = {initiator.node.node_id()};
  const auto added =
      zetta::data::add_dataset_rows(initiator.node, std::move(rows), add);
  ASSERT_TRUE(added.ok()) << added.error;
  ASSERT_TRUE(patiently([&] {
    first.node.tick();
    second.node.tick();
    for (const auto &dataset : root.node.datasets()) {
      if (dataset.database == "datasets" && dataset.name == "history" &&
          dataset.rows == 168 && !dataset.degraded) {
        return true;
      }
    }
    return false;
  }));

  const auto *methodology = zetta::ml::find_methodology("M-4");
  zetta::ml::ShapeRequest request;
  request.hints = {{"num_trees", 6}, {"max_depth", 2}, {"backtest_folds", 2}};
  auto graph = methodology->build_shape(request);
  std::string error;
  const auto parsed = zetta::ml::parse_forecast_pipeline(graph, error);
  ASSERT_TRUE(parsed) << error;
  const auto oracle = zetta::ml::train_forecast(series.times, series.values,
                                                parsed->params.forecast);
  ASSERT_TRUE(oracle.ok()) << oracle.status.message;
  const auto project = initiator.node.create_project("forecast-launch");
  ASSERT_TRUE(project);
  ASSERT_TRUE(project->create_graph("forecast", std::move(graph)));
  zetta::api::Registry registry;
  zetta::api::register_core_capabilities(registry);
  zetta::api::BackgroundActions background;

  std::jthread peer([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      root.node.tick();
      std::this_thread::sleep_for(5ms);
    }
  });
  const auto launched =
      call(registry, initiator.node, background, "run",
           {{"project", "forecast-launch"}, {"graph", "forecast"}});
  ASSERT_TRUE(launched.ok) << launched.error.message;
  const std::string run_id = launched.value.at("run_id");
  ASSERT_TRUE(patiently([&] {
    initiator.node.tick();
    background.prune();
    const auto run = initiator.node.find_run(run_id);
    return run && (run->status() == zetta::pipeline::RunStatus::Succeeded ||
                   run->status() == zetta::pipeline::RunStatus::Failed);
  }));
  const auto run = initiator.node.find_run(run_id);
  ASSERT_TRUE(run);
  ASSERT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded)
      << run->metadata();
  zetta::api::ApiResult report;
  ASSERT_TRUE(patiently([&] {
    initiator.node.tick();
    background.prune();
    report = call(registry, initiator.node, background, "get_run_report",
                  {{"run_id", run_id}});
    return report.ok && report.value.contains("forecast") &&
           report.value["forecast"].size() == 7;
  })) << report.error.message;
  EXPECT_NEAR(report.value["model"]["metrics"]["backtest_mae"].get<double>(),
              oracle.metrics.mean.mae, 1e-8);
  EXPECT_EQ(report.value["dataset"]["rows_read"], 168);
  for (std::size_t index = 0; index < oracle.forecast.size(); ++index) {
    EXPECT_EQ(report.value["forecast"][index]["timestamp"],
              oracle.forecast[index].timestamp);
    EXPECT_NEAR(report.value["forecast"][index]["point_forecast"].get<double>(),
                oracle.forecast[index].point, 1e-8);
  }
  const auto limited =
      call(registry, initiator.node, background, "get_run_report",
           {{"run_id", run_id}, {"limit", 2}});
  ASSERT_TRUE(limited.ok) << limited.error.message;
  ASSERT_EQ(limited.value["forecast"].size(), 2u);
  EXPECT_EQ(limited.value["forecast"][0], report.value["forecast"][0]);
  EXPECT_EQ(limited.value["forecast"][1], report.value["forecast"][1]);
  peer.request_stop();
  peer.join();
  EXPECT_EQ(root.node.models().size(), 1u);
  EXPECT_EQ(initiator.node.models().size(), 1u);
  EXPECT_EQ(root.node.models().front()->id(),
            initiator.node.models().front()->id());
}

TEST(ForecastLaunch, MalformedPeerPayloadIsRefusedObservably) {
  ForecastMember member(47632, {});
  zetta::compute::CoordinatedLaunchRequest request;
  request.id = "invalid-forecast";
  request.kind = "forecast";
  request.initiator = member.id;
  request.participants = {member.id};
  request.payload = {{"graph", 42}};
  zetta::cluster::Signal signal;
  signal.kind = zetta::cluster::SignalKind::Task;
  signal.method = "run.launch";
  signal.payload = zetta::compute::encode_coordinated_launch(request);
  member.node.coordinated_launch()->on_task(signal);
  member.node.tick();
  bool refused = false;
  for (const auto &span : member.node.span_recorder().snapshot()) {
    if (span.name == "run.launch:forecast" && !span.ok && !span.error.empty()) {
      refused = true;
    }
  }
  EXPECT_TRUE(refused);
  EXPECT_TRUE(member.node.models().empty());
}
