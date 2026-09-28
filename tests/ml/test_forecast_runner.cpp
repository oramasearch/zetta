#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/ml/forecast_runner.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

constexpr double kPi = 3.14159265358979323846;

std::vector<double> days(const std::size_t count) {
  std::vector<double> times(count);
  std::iota(times.begin(), times.end(), 0.0);
  return times;
}

zetta::ml::ForecastSeries make_series(const std::int64_t key,
                                      const double offset) {
  zetta::ml::ForecastSeries series;
  series.key = key;
  series.times = days(168);
  series.values.resize(series.times.size());
  for (std::size_t day = 0; day < series.values.size(); ++day) {
    series.values[day] =
        offset + 0.4 * static_cast<double>(day) +
        12.0 * std::sin(2.0 * kPi * static_cast<double>(day) / 7.0);
  }
  return series;
}

zetta::ml::ForecastRunParams run_params() {
  zetta::ml::ForecastRunParams params;
  params.forecast.gbdt.num_trees = 12;
  params.forecast.gbdt.max_depth = 3;
  params.forecast.gbdt.learning_rate = 0.2;
  params.output_database = "forecast_test";
  params.output_table = "result";
  return params;
}

zetta::cluster::ClusterConfig
cluster_config(const std::uint16_t port,
               const std::vector<std::string> &seeds) {
  zetta::cluster::ClusterConfig config;
  config.listen_host = "127.0.0.1";
  config.listen_port = port;
  config.port_range = 1;
  config.seeds = seeds;
  return config;
}

template <class Predicate> bool wait_for(Predicate predicate) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(25ms);
  }
  return false;
}

struct Member {
  Member(const std::string &id, const std::uint16_t port,
         std::vector<std::string> seeds,
         std::vector<zetta::ml::ForecastSeries> owned)
      : node(zetta::NodeConfig{.data_dir = temporary.path().string()}),
        cluster(id, cluster_config(port, seeds)), coordinator(cluster, 2),
        series(std::move(owned)) {
    EXPECT_TRUE(node.start());
    job = node.create_job(zetta::compute::JobKind::Train);
    EXPECT_TRUE(job);
  }

  zetta::test::TempDir temporary;
  zetta::Node node;
  zetta::cluster::Cluster cluster;
  zetta::compute::JobCoordinator coordinator;
  std::shared_ptr<zetta::compute::ComputeJob> job;
  std::vector<zetta::ml::ForecastSeries> series;
};

std::vector<std::unique_ptr<Member>>
make_cluster(const std::vector<std::vector<zetta::ml::ForecastSeries>> &shards,
             const std::uint16_t base_port) {
  std::vector<std::unique_ptr<Member>> members;
  for (std::size_t index = 0; index < shards.size(); ++index) {
    std::vector<std::string> seeds;
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
    }
    members.push_back(std::make_unique<Member>(
        "forecast-member-" + std::to_string(base_port + index),
        static_cast<std::uint16_t>(base_port + index), seeds, shards[index]));
  }
  for (auto &member : members)
    member->cluster.start();
  EXPECT_TRUE(wait_for([&] {
    return std::ranges::all_of(members, [&](const auto &member) {
      return member->cluster.peer_count() == members.size() - 1;
    });
  }));
  for (auto &member : members)
    member->coordinator.start();
  return members;
}

std::vector<zetta::ml::ForecastRunResult>
run_cluster(std::vector<std::unique_ptr<Member>> &members,
            const zetta::ml::ForecastRunParams &params) {
  std::vector<zetta::ml::ForecastRunResult> results(members.size());
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < members.size(); ++index) {
    threads.emplace_back([&, index] {
      results[index] = zetta::ml::run_forecast_job(
          members[index]->coordinator, *members[index]->job,
          members[index]->series, params, nullptr, "datasets.history", 8s);
    });
  }
  for (std::thread &thread : threads)
    thread.join();
  return results;
}

struct TableHost {
  explicit TableHost(const std::uint16_t port)
      : node(zetta::NodeConfig{.data_dir = temporary.path().string()}),
        cluster("forecast-table-node", cluster_config(port, {})) {
    EXPECT_TRUE(node.start());
    cluster.start();
    node.attach_cluster(cluster, 2);
  }

  ~TableHost() { node.stop(); }

  zetta::test::TempDir temporary;
  zetta::Node node;
  zetta::cluster::Cluster cluster;
};

} // namespace

TEST(ForecastPartitions, SeveralMembersMatchTheSingleMemberModel) {
  const zetta::ml::ForecastSeries first = make_series(1, 100.0);
  const zetta::ml::ForecastSeries second = make_series(2, 400.0);
  const zetta::ml::ForecastRunParams params = run_params();

  const zetta::ml::AllReduce identity =
      [](const std::string &, const std::vector<double> &local,
         const zetta::ml::GbdtReduce) { return local; };
  const zetta::ml::ForecastResult oracle = zetta::ml::train_forecast_partitions(
      {first, second}, params.forecast, identity, "forecast-oracle");
  ASSERT_TRUE(oracle.ok()) << oracle.status.message;

  std::vector<std::unique_ptr<Member>> members =
      make_cluster({{first}, {second}}, 47560);
  const std::vector<zetta::ml::ForecastRunResult> results =
      run_cluster(members, params);

  for (const zetta::ml::ForecastRunResult &result : results) {
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.result.model.to_json().dump(),
              results.front().result.model.to_json().dump());
    EXPECT_NEAR(result.result.metrics.mean.mae, oracle.metrics.mean.mae, 1e-9);
    EXPECT_NEAR(result.result.metrics.mean.forecast_value_added,
                oracle.metrics.mean.forecast_value_added, 1e-9);
  }
  const auto root = std::ranges::find_if(
      results, [](const zetta::ml::ForecastRunResult &result) {
        return result.is_root;
      });
  ASSERT_NE(root, results.end());
  EXPECT_EQ(root->result.forecast.size(), 14u);
  ASSERT_EQ(root->result.forecast.size(), oracle.forecast.size());
  for (std::size_t row = 0; row < oracle.forecast.size(); ++row) {
    EXPECT_EQ(root->result.forecast[row].series, oracle.forecast[row].series);
    EXPECT_DOUBLE_EQ(root->result.forecast[row].timestamp,
                     oracle.forecast[row].timestamp);
    EXPECT_NEAR(root->result.forecast[row].point, oracle.forecast[row].point,
                1e-9);
    EXPECT_NEAR(root->result.forecast[row].lower, oracle.forecast[row].lower,
                1e-9);
    EXPECT_NEAR(root->result.forecast[row].upper, oracle.forecast[row].upper,
                1e-9);
  }
}

TEST(ForecastPartitions, ASeriesSplitAcrossMembersIsRefused) {
  zetta::ml::ForecastSeries first_half = make_series(7, 100.0);
  zetta::ml::ForecastSeries second_half = first_half;
  first_half.times.resize(84);
  first_half.values.resize(84);
  second_half.times.erase(second_half.times.begin(),
                          second_half.times.begin() + 84);
  second_half.values.erase(second_half.values.begin(),
                           second_half.values.begin() + 84);

  std::vector<std::unique_ptr<Member>> members =
      make_cluster({{first_half}, {second_half}}, 47570);
  const std::vector<zetta::ml::ForecastRunResult> results =
      run_cluster(members, run_params());
  for (const zetta::ml::ForecastRunResult &result : results) {
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("series spans members"), std::string::npos);
  }
}

TEST(ForecastRunner, AResolvedTableWritesForecastModelOutputsAndLineage) {
  TableHost host(47580);
  std::vector<nlohmann::json> rows;
  for (const zetta::ml::ForecastSeries &series :
       {make_series(101, 100.0), make_series(202, 300.0)}) {
    for (std::size_t row = 0; row < series.times.size(); ++row) {
      rows.push_back({{"series_id", series.key},
                      {"day", series.times[row]},
                      {"demand", series.values[row]}});
    }
  }
  zetta::data::AddDatasetOptions add;
  add.database = "demand";
  add.table_name = "history";
  add.partitioned = true;
  add.members = {host.node.node_id()};
  const zetta::data::AddDatasetResult added =
      zetta::data::add_dataset_rows(host.node, std::move(rows), add);
  ASSERT_TRUE(added.ok()) << added.error;
  ASSERT_TRUE(added.table);

  zetta::data::ResolvedDataset dataset;
  dataset.table_id = added.table->id();
  dataset.database = "demand";
  dataset.table = "history";
  dataset.rows = 336;

  zetta::ml::ForecastRunParams params = run_params();
  params.series_column = "series_id";
  params.time_column = "day";
  params.target_column = "demand";
  params.model_name = "weekly demand";
  zetta::compute::CpuDevice device;
  const zetta::ml::ForecastRunResult result =
      zetta::ml::run_forecast_table(host.node, device, dataset, params);

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.rows_read, 336u);
  EXPECT_EQ(result.result.forecast.size(), 14u);
  EXPECT_EQ(result.forecast_table, "forecast_test.result");
  EXPECT_EQ(result.lineage.sources,
            (std::vector<std::string>{"dataset:demand.history"}));
  ASSERT_EQ(result.outputs.size(), 2u);

  const std::shared_ptr<zetta::ml::Model> model =
      host.node.find_model(result.model_id);
  ASSERT_TRUE(model);
  EXPECT_EQ(model->kind(), "forecast");
  EXPECT_TRUE(model->has_weights_body());
  EXPECT_TRUE(model->metrics().contains("backtest_mae"));
  EXPECT_TRUE(model->metrics().contains("fold_0_mae"));
  EXPECT_EQ(model->feature_schema().at("baseline_comparison"),
            result.result.metrics.comparison);

  const std::shared_ptr<zetta::data::Database> output_database =
      host.node.find_database("forecast_test");
  ASSERT_TRUE(output_database);
  const std::shared_ptr<zetta::data::Table> output_table =
      output_database->find_table("result");
  ASSERT_TRUE(output_table);
  EXPECT_EQ(output_table->store().size(), 14u);
}
