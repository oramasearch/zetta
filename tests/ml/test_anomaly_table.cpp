// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/anomaly_table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

namespace {

struct AnomalyTableHost {
  AnomalyTableHost()
      : node(node_config()), cluster("anomaly-table-node", cluster_config()) {
    node.start();
    cluster.start();
    node.attach_cluster(cluster, 2);
  }

  ~AnomalyTableHost() {
    node.stop();
    cluster.stop();
  }

  zetta::NodeConfig node_config() const {
    zetta::NodeConfig config;
    config.data_dir = directory.path().string();
    return config;
  }

  static zetta::cluster::ClusterConfig cluster_config() {
    zetta::cluster::ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = 47650;
    config.port_range = 1;
    return config;
  }

  zetta::test::TempDir directory;
  zetta::Node node;
  zetta::cluster::Cluster cluster;
};

std::vector<nlohmann::json> anomaly_rows() {
  std::vector<nlohmann::json> rows;
  for (std::size_t index = 0; index < 100; ++index) {
    double latency = static_cast<double>((index * 7) % 19) / 10.0;
    double errors = static_cast<double>((index * 3) % 11) / 10.0;
    if (index == 41) {
      latency = 80.0;
    }
    if (index == 82) {
      errors = 90.0;
    }
    rows.push_back({{"request_id", static_cast<std::int64_t>(index + 1)},
                    {"latency", latency},
                    {"errors", errors}});
  }
  return rows;
}

} // namespace

TEST(AnomalyTableRunner, WritesQueryableOutputsAndRunLineageWithoutALabel) {
  AnomalyTableHost host;
  zetta::data::AddDatasetOptions add_options;
  add_options.database = "operations";
  add_options.table_name = "requests";
  add_options.partitioned = true;
  add_options.members = {host.node.node_id()};
  const zetta::data::AddDatasetResult added =
      zetta::data::add_dataset_rows(host.node, anomaly_rows(), add_options);
  ASSERT_TRUE(added.ok()) << added.error;
  ASSERT_TRUE(added.table);

  zetta::data::ResolvedDataset dataset;
  dataset.table_id = added.table->id();
  dataset.database = "operations";
  dataset.table = "requests";
  dataset.rows = 100;
  dataset.contract.entity_column = "request_id";
  dataset.contract.feature_columns = {"latency", "errors"};
  ASSERT_TRUE(dataset.contract.label_column.empty());

  const std::shared_ptr<zetta::pipeline::Run> run =
      host.node.create_run("anomaly-project", "anomaly-graph");
  ASSERT_TRUE(run);
  zetta::ml::AnomalyParams params;
  params.tree_count = 32;
  params.subsample_size = 32;
  params.threshold.contamination.reset();
  params.threshold.alert_budget = 2;
  const zetta::ml::AnomalyTableResult result =
      zetta::ml::run_anomaly_on_table(host.node, dataset, params, run.get());
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.records.size(), 100u);
  EXPECT_EQ(result.report.flagged, 2u);
  EXPECT_FALSE(result.anomaly_table.empty());
  EXPECT_FALSE(result.report_table.empty());

  const std::shared_ptr<zetta::ml::Model> model =
      host.node.find_model(result.model_id);
  ASSERT_TRUE(model);
  EXPECT_EQ(model->kind(), "anomaly");
  EXPECT_TRUE(model->has_weights_body());

  const std::shared_ptr<zetta::data::Database> outputs =
      host.node.find_database("anomaly_results");
  ASSERT_TRUE(outputs);
  const std::string anomaly_name =
      result.anomaly_table.substr(result.anomaly_table.find('.') + 1);
  const std::string report_name =
      result.report_table.substr(result.report_table.find('.') + 1);
  const std::shared_ptr<zetta::data::Table> anomaly_table =
      outputs->find_table(anomaly_name);
  const std::shared_ptr<zetta::data::Table> report_table =
      outputs->find_table(report_name);
  ASSERT_TRUE(anomaly_table);
  ASSERT_TRUE(report_table);
  EXPECT_EQ(anomaly_table->row_count(), 100u);
  EXPECT_EQ(report_table->row_count(), 1u);

  EXPECT_EQ(run->lineage().sources,
            (std::vector<std::string>{"operations.requests"}));
  EXPECT_EQ(run->lineage().steps,
            (std::vector<std::string>{"anomaly:train", "anomaly:score"}));
  EXPECT_EQ(run->outputs().size(), 3u);
  EXPECT_EQ(run->outputs()[0].ref, result.model_id);
  EXPECT_EQ(run->outputs()[1].ref, result.anomaly_table);
  EXPECT_EQ(run->outputs()[2].ref, result.report_table);
}
