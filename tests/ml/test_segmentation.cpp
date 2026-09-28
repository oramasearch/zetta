#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/segmentation.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::ml::SegmentationParams;
using zetta::ml::SegmentationResult;
using zetta::ml::SegmentationRow;

struct PlantedData {
    std::vector<SegmentationRow> rows;
    std::vector<int> truth;
};

PlantedData planted_groups(std::size_t per_group = 60) {
    PlantedData data;
    std::mt19937_64 generator(1042);
    std::normal_distribution<double> noise(0.0, 0.15);
    const std::vector<std::pair<double, double>> centers = {
        {8.0, 2.0}, {0.0, 10.0}, {0.5, 1.5}};
    for (std::size_t cluster = 0; cluster < centers.size(); ++cluster) {
        for (std::size_t row = 0; row < per_group; ++row) {
            const std::size_t index = cluster * per_group + row;
            data.rows.push_back({
                .entity = static_cast<std::int64_t>(index + 1),
                .values = {{"failed_payments",
                            centers[cluster].first + noise(generator)},
                           {"tenure",
                            centers[cluster].second + noise(generator)},
                           {"account_id",
                            static_cast<std::int64_t>(index + 1000)},
                           {"target", Json{{"must_not_be_read", true}}}},
            });
            data.truth.push_back(static_cast<int>(cluster));
        }
    }
    return data;
}

SegmentationParams planted_params() {
    SegmentationParams params;
    params.minimum_k = 2;
    params.maximum_k = 5;
    params.restarts = 4;
    params.max_iterations = 60;
    params.seed = 1042;
    params.feature_columns = {"failed_payments", "tenure", "account_id",
                              "target"};
    params.entity_column = "entity";
    params.label_column = "target";
    params.silhouette_sample_size = 512;
    return params;
}

std::vector<int> assignment_labels(const SegmentationResult &result) {
    std::vector<int> labels;
    labels.reserve(result.assignments.size());
    for (const auto &assignment : result.assignments) {
        labels.push_back(assignment.segment_id);
    }
    return labels;
}

bool wait_for_cluster(
    const std::vector<std::unique_ptr<struct ClusterMember>> &members);

struct ClusterMember {
    ClusterMember(std::string id, std::uint16_t port,
                  const std::vector<std::string> &seeds)
        : node(node_config(temp_dir)),
          cluster(std::move(id), cluster_config(port, seeds)),
          coordinator(cluster, 2) {
        node.start();
    }

    ~ClusterMember() {
        coordinator.stop();
        cluster.stop();
        node.stop();
    }

    static NodeConfig node_config(const zetta::test::TempDir &directory) {
        NodeConfig config;
        config.data_dir = directory.path().string();
        return config;
    }

    static ClusterConfig cluster_config(std::uint16_t port,
                                        const std::vector<std::string> &seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = seeds;
        return config;
    }

    zetta::test::TempDir temp_dir;
    Node node;
    Cluster cluster;
    zetta::compute::JobCoordinator coordinator;
};

bool wait_for_cluster(
    const std::vector<std::unique_ptr<ClusterMember>> &members) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        const bool ready = std::all_of(
            members.begin(), members.end(), [&](const auto &member) {
                return member->cluster.peer_count() == members.size() - 1;
            });
        if (ready)
            return true;
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

std::vector<std::unique_ptr<ClusterMember>>
make_cluster(std::size_t count, std::uint16_t base_port) {
    std::vector<std::unique_ptr<ClusterMember>> members;
    for (std::size_t index = 0; index < count; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
        }
        members.push_back(std::make_unique<ClusterMember>(
            "segment-member-" + std::to_string(index),
            static_cast<std::uint16_t>(base_port + index), seeds));
    }
    for (auto &member : members)
        member->cluster.start();
    EXPECT_TRUE(wait_for_cluster(members));
    for (auto &member : members)
        member->coordinator.start();
    return members;
}

struct AttachedNode {
    explicit AttachedNode(std::uint16_t port,
                          const std::vector<std::string> &seeds = {})
        : node(node_config(temp_dir)),
          cluster("segment-runner-" + std::to_string(port),
                  cluster_config(port, seeds)) {
        node.start();
        cluster.start();
        node.attach_cluster(cluster, 2);
    }

    ~AttachedNode() {
        node.stop();
    }

    static NodeConfig node_config(const zetta::test::TempDir &directory) {
        NodeConfig config;
        config.data_dir = directory.path().string();
        return config;
    }

    static ClusterConfig cluster_config(std::uint16_t port,
                                        const std::vector<std::string> &seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = seeds;
        return config;
    }

    zetta::test::TempDir temp_dir;
    Node node;
    Cluster cluster;
};

} // namespace

TEST(Segmentation, ThreeGaussianGroupsAreRecoveredAndKIsSelected) {
    const PlantedData data = planted_groups();
    const SegmentationResult result =
        zetta::ml::train_segmentation(data.rows, planted_params());
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.metrics.chosen_k, 3);
    EXPECT_GT(
        zetta::ml::adjusted_rand_index(data.truth, assignment_labels(result)),
        0.9);
    EXPECT_GT(result.metrics.silhouette, 0.5);
    EXPECT_EQ(result.metrics.tried.size(), 4u);

    bool names_signal = false;
    for (const auto &profile : result.profiles) {
        names_signal = names_signal ||
                       std::any_of(profile.distinguishing_features.begin(),
                                   profile.distinguishing_features.end(),
                                   [](const std::string &feature) {
                                       return feature.find("failed_payments") !=
                                              std::string::npos;
                                   });
    }
    EXPECT_TRUE(names_signal);
}

TEST(Segmentation, TargetAndIdentifierColumnsAreNeverUsed) {
    const PlantedData data = planted_groups(20);
    const SegmentationResult result =
        zetta::ml::train_segmentation(data.rows, planted_params());
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_TRUE(std::ranges::find(result.model.excluded_columns, "target") !=
                result.model.excluded_columns.end());
    EXPECT_TRUE(
        std::ranges::find(result.model.excluded_columns, "account_id") !=
        result.model.excluded_columns.end());
    for (const auto &feature : result.model.features) {
        EXPECT_NE(feature.source, "target");
        EXPECT_NE(feature.source, "account_id");
    }
}

TEST(Segmentation, SameSeedReproducesModelMetricsAndAssignments) {
    const auto data = planted_groups(20);
    const auto first =
        zetta::ml::train_segmentation(data.rows, planted_params());
    const auto second =
        zetta::ml::train_segmentation(data.rows, planted_params());
    ASSERT_TRUE(first.ok()) << first.status.message;
    ASSERT_TRUE(second.ok()) << second.status.message;
    EXPECT_EQ(first.model.to_json(), second.model.to_json());
    EXPECT_EQ(first.metrics.silhouette, second.metrics.silhouette);
    EXPECT_EQ(first.metrics.stability, second.metrics.stability);
    EXPECT_EQ(assignment_labels(first), assignment_labels(second));
    for (std::size_t index = 0; index < first.assignments.size(); ++index)
        EXPECT_EQ(first.assignments[index].distance_to_center,
                  second.assignments[index].distance_to_center);
}

TEST(SegmentationPreprocessing, HeavyTailUsesExactGlobalMedianAndIqr) {
    std::vector<SegmentationRow> rows;
    for (std::int64_t entity = 0; entity < 100; ++entity)
        rows.push_back({entity, {{"amount", static_cast<double>(entity)}}});
    rows.back().values["amount"] = 1e9;
    SegmentationParams params;
    params.feature_columns = {"amount"};
    params.maximum_k = 2;
    const auto result = zetta::ml::train_segmentation(rows, params);
    ASSERT_TRUE(result.ok()) << result.status.message;
    ASSERT_EQ(result.model.features.size(), 1u);
    EXPECT_TRUE(result.model.features.front().robust);
    EXPECT_DOUBLE_EQ(result.model.features.front().location, 49.5);
    EXPECT_DOUBLE_EQ(result.model.features.front().scale, 49.5);
}

TEST(SegmentationPreprocessing, RejectsNonFiniteFeaturesAndParameters) {
    auto data = planted_groups(10);
    data.rows.front().values["tenure"] =
        std::numeric_limits<double>::infinity();
    EXPECT_FALSE(
        zetta::ml::train_segmentation(data.rows, planted_params()).ok());
    auto params = planted_params();
    params.minimum_segment_share = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(
        zetta::ml::train_segmentation(planted_groups(10).rows, params).ok());
}

TEST(SegmentationPreprocessing, DuplicateEntityKeysAreRejected) {
    auto data = planted_groups(10);
    data.rows.back().entity = data.rows.front().entity;
    const auto result =
        zetta::ml::train_segmentation(data.rows, planted_params());
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.status.message.find("unique globally"), std::string::npos);
}

TEST(SegmentationPreprocessing,
     ConstantDataFailsWithoutPublishingInvalidMetrics) {
    std::vector<SegmentationRow> rows;
    for (std::int64_t entity = 0; entity < 8; ++entity)
        rows.push_back({entity, {{"amount", 1.0}}});
    SegmentationParams params;
    params.feature_columns = {"amount"};
    params.maximum_k = 2;
    EXPECT_FALSE(zetta::ml::train_segmentation(rows, params).ok());
}

TEST(SegmentationPreprocessing, RareBucketCannotCollideWithLiteralOther) {
    std::vector<SegmentationRow> rows;
    for (std::int64_t entity = 0; entity < 40; ++entity) {
        rows.push_back({entity,
                        {{"amount", entity < 20 ? 0.0 : 10.0},
                         {"plan", entity == 39 ? "rare" : "other"},
                         {"notes", "repeated free text"}}});
    }
    SegmentationParams params;
    params.feature_columns = {"amount", "plan", "notes", "customer_key",
                              "target"};
    params.categorical_columns = {"plan"};
    params.rare_category_share = 0.1;
    params.maximum_k = 2;
    const auto result = zetta::ml::train_segmentation(rows, params);
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_TRUE(
        std::ranges::any_of(result.model.features, [](const auto &feature) {
            return feature.encoded_name == "plan=other" && !feature.rare_bucket;
        }));
    EXPECT_TRUE(
        std::ranges::any_of(result.model.features, [](const auto &feature) {
            return feature.encoded_name == "plan=_other" && feature.rare_bucket;
        }));
    EXPECT_FALSE(
        std::ranges::any_of(result.model.features, [](const auto &feature) {
            return feature.source == "notes" ||
                   feature.source == "customer_key" ||
                   feature.source == "target";
        }));
}

TEST(KMeans, TinyClusterIsRejectedByMinimumShare) {
    PlantedData data = planted_groups(48);
    data.rows.resize(100);
    data.truth.resize(100);
    SegmentationParams params = planted_params();
    params.minimum_k = 2;
    params.maximum_k = 3;
    params.minimum_segment_share = 0.10;

    const SegmentationResult result =
        zetta::ml::train_segmentation(data.rows, params);
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.metrics.chosen_k, 2);
    const auto k_three =
        std::ranges::find_if(result.metrics.tried, [](const auto &metric) {
            return metric.k == 3;
        });
    ASSERT_NE(k_three, result.metrics.tried.end());
    EXPECT_FALSE(k_three->satisfies_minimum_share);
}

TEST(SegmentationPreprocessing,
     LowCardinalityCategoriesAreEncodedAndTextIsExcluded) {
    std::vector<SegmentationRow> rows;
    for (std::int64_t entity = 0; entity < 30; ++entity) {
        rows.push_back(
            {entity,
             {{"amount", entity < 15 ? 0.0 : 10.0},
              {"plan", entity % 3 == 0 ? "team" : "solo"},
              {"notes", "free text value number " + std::to_string(entity)}}});
    }
    SegmentationParams params;
    params.minimum_k = 2;
    params.maximum_k = 2;
    params.restarts = 2;
    params.feature_columns = {"amount", "plan", "notes"};
    params.categorical_columns = {"plan"};
    const SegmentationResult result =
        zetta::ml::train_segmentation(rows, params);
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_TRUE(std::ranges::find(result.model.excluded_columns, "notes") !=
                result.model.excluded_columns.end());
    EXPECT_TRUE(
        std::ranges::any_of(result.model.features, [](const auto &feature) {
            return feature.encoded_name == "plan=solo";
        }));
}

TEST(SegmentationCluster, ThreePartitionsMatchTheSingleMemberCentres) {
    const PlantedData data = planted_groups(30);
    const SegmentationResult oracle =
        zetta::ml::train_segmentation(data.rows, planted_params());
    ASSERT_TRUE(oracle.ok()) << oracle.status.message;

    constexpr std::size_t member_count = 3;
    std::vector<std::vector<SegmentationRow>> shards(member_count);
    for (std::size_t index = 0; index < data.rows.size(); ++index) {
        shards[index % member_count].push_back(data.rows[index]);
    }
    auto members = make_cluster(member_count, 47680);
    std::vector<SegmentationResult> results(member_count);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < member_count; ++index) {
        threads.emplace_back([&, index] {
            const auto job =
                members[index]->node.create_job(zetta::compute::JobKind::Train);
            members[index]->coordinator.run(
                *job, [&](zetta::compute::JobRound &round) {
                    zetta::ml::SegmentationCollectives collectives;
                    collectives.allreduce =
                        [&](const std::string &tag,
                            const std::vector<double> &local,
                            zetta::ml::SegmentationReduce operation) {
                            zetta::cluster::CollectiveOp mapped =
                                zetta::cluster::CollectiveOp::Sum;
                            if (operation ==
                                zetta::ml::SegmentationReduce::Minimum) {
                                mapped = zetta::cluster::CollectiveOp::Minimum;
                            } else if (operation ==
                                       zetta::ml::SegmentationReduce::Maximum) {
                                mapped = zetta::cluster::CollectiveOp::Maximum;
                            }
                            return round.allreduce(tag, local, mapped, 8s);
                        };
                    collectives.gather = [&](const std::string &tag,
                                             const std::vector<double> &local) {
                        return round.gather(tag, local, 8s);
                    };
                    collectives.broadcast =
                        [&](const std::string &tag,
                            const std::vector<double> &value) {
                            return round.broadcast(tag, value, 8s);
                        };
                    results[index] = zetta::ml::train_segmentation(
                        shards[index], planted_params(), collectives,
                        "segment-partitions");
                    return zetta::compute::JobCoordinator::Outcome{
                        results[index].ok(), results[index].status.message};
                });
        });
    }
    for (std::thread &thread : threads)
        thread.join();

    for (const SegmentationResult &result : results) {
        ASSERT_TRUE(result.ok()) << result.status.message;
        ASSERT_EQ(result.metrics.chosen_k, 3);
        ASSERT_EQ(result.model.centers.size(), oracle.model.centers.size());
        EXPECT_EQ(result.model.to_json().dump(),
                  results.front().model.to_json().dump());
        for (std::size_t cluster = 0; cluster < result.model.centers.size();
             ++cluster) {
            for (std::size_t feature = 0;
                 feature < result.model.centers[cluster].size(); ++feature) {
                EXPECT_NEAR(result.model.centers[cluster][feature],
                            oracle.model.centers[cluster][feature], 1e-9);
            }
        }
    }
    std::vector<int> distributed_labels(data.rows.size());
    for (const auto &result : results) {
        for (const auto &assignment : result.assignments)
            distributed_labels[static_cast<std::size_t>(
                assignment.entity - 1)] = assignment.segment_id;
    }
    EXPECT_GT(zetta::ml::adjusted_rand_index(data.truth, distributed_labels),
              0.9);
}

TEST(SegmentationRunner,
     ResolvedTableWritesAssignmentsProfilesModelAndLineage) {
    AttachedNode host(47690);
    const PlantedData planted = planted_groups(20);
    std::vector<Json> rows;
    for (std::size_t index = 0; index < planted.rows.size(); ++index) {
        Json row = planted.rows[index].values;
        row["customer_id"] =
            (std::int64_t{1} << 54) + planted.rows[index].entity;
        row["target"] = planted.truth[index] == 0 ? 1 : 0;
        rows.push_back(std::move(row));
    }
    zetta::data::AddDatasetOptions add;
    add.database = "customers";
    add.table_name = "entities";
    add.partitioned = true;
    add.members = {host.node.node_id()};
    const zetta::data::AddDatasetResult landed =
        zetta::data::add_dataset_rows(host.node, rows, add);
    ASSERT_TRUE(landed.ok()) << landed.error;

    zetta::data::ResolvedDataset dataset = zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("customers.entities"), {},
        zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(dataset.ok()) << dataset.error;
    dataset.contract.entity_column = "customer_id";
    // A malformed manual contract must still exclude a target before lookup.
    // This named target is deliberately absent from every row.
    dataset.contract.label_column = "never_read";
    dataset.contract.feature_columns.push_back("never_read");

    SegmentationParams params = planted_params();
    params.minimum_k = 2;
    params.maximum_k = 4;
    zetta::ml::SegmentationRunOptions options;
    options.project = "segmentation-test";
    options.graph = "segmentation-graph";
    options.model_name = "customer_segments";
    const zetta::ml::SegmentationRunResult result =
        zetta::ml::run_segmentation(host.node, dataset, params, options);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_TRUE(result.is_root);
    EXPECT_EQ(result.segmentation.assignments.size(), rows.size());
    EXPECT_EQ(result.segmentation.assignments.front().entity,
              (std::int64_t{1} << 54) + 1);

    const auto model = host.node.find_model(result.model_id);
    ASSERT_TRUE(model);
    EXPECT_EQ(model->kind(), "segmentation");
    EXPECT_TRUE(model->metrics().contains("silhouette"));

    const auto run = host.node.find_run(result.run_id);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);
    EXPECT_EQ(run->lineage().sources,
              (std::vector<std::string>{"dataset:customers.entities"}));
    EXPECT_EQ(run->outputs().size(), 3u);

    const auto output_database =
        host.node.find_database("segmentation_results");
    ASSERT_TRUE(output_database);
    const auto assignment_table = output_database->find_table(
        result.assignment_table.substr(result.assignment_table.find('.') + 1));
    const auto profile_table = output_database->find_table(
        result.profile_table.substr(result.profile_table.find('.') + 1));
    ASSERT_TRUE(assignment_table);
    ASSERT_TRUE(profile_table);
    EXPECT_EQ(assignment_table->row_count(), rows.size());
    EXPECT_EQ(profile_table->row_count(),
              static_cast<std::size_t>(result.segmentation.metrics.chosen_k));
}

TEST(SegmentationCluster, RunnerReadsThreeStoredOwnedPartitions) {
    const auto planted = planted_groups(20);
    auto params = planted_params();
    params.maximum_k = 3;
    params.restarts = 2;
    const auto oracle = zetta::ml::train_segmentation(planted.rows, params);
    ASSERT_TRUE(oracle.ok()) << oracle.status.message;

    std::vector<std::unique_ptr<AttachedNode>> members;
    for (int index = 0; index < 3; ++index) {
        std::vector<std::string> seeds;
        for (int earlier = 0; earlier < index; ++earlier)
            seeds.push_back("127.0.0.1:" + std::to_string(47700 + earlier));
        members.push_back(std::make_unique<AttachedNode>(47700 + index, seeds));
    }
    bool connected = false;
    for (int attempt = 0; attempt < 400; ++attempt) {
        connected = std::ranges::all_of(members, [](const auto &member) {
            return member->cluster.peer_count() == 2;
        });
        if (connected)
            break;
        std::this_thread::sleep_for(25ms);
    }
    ASSERT_TRUE(connected);
    std::vector<std::shared_ptr<zetta::data::Table>> tables;
    std::vector<std::string> blobs;
    for (std::size_t index = 0; index < members.size(); ++index) {
        auto database = members[index]->node.create_database("partitioned");
        tables.push_back(
            database->create_table("entities", std::nullopt, true));
        std::vector<zetta::data::PartitionRow> rows;
        for (std::size_t row = index; row < planted.rows.size();
             row += members.size()) {
            Json document = planted.rows[row].values;
            document["entity"] = planted.rows[row].entity;
            rows.push_back({std::to_string(row), std::move(document)});
        }
        const auto body = zetta::data::serialize_partition_body(rows);
        ASSERT_TRUE(body.ok()) << body.error;
        const auto blob =
            database->large_objects().import_bytes("entities", body.bytes);
        ASSERT_TRUE(blob);
        blobs.push_back(blob->id);
    }
    std::vector<zetta::data::ResolvedDataset> datasets;
    for (std::size_t member = 0; member < members.size(); ++member) {
        for (std::size_t partition = 0; partition < members.size();
             ++partition) {
            zetta::data::DatasetPartitionValue value;
            value.table = tables[member]->id();
            value.blob_ref = blobs[partition];
            value.row_range = {partition * 20, (partition + 1) * 20};
            value.owner_nodes = {members[partition]->node.node_id()};
            ASSERT_TRUE(members[member]->node.create_dataset_partition(value));
        }
        zetta::data::DatasetManifestValue manifest;
        manifest.table = tables[member]->id();
        manifest.partitions = 3;
        manifest.rows = 60;
        tables[member]->set_partition_manifest(manifest);
        zetta::data::ResolvedDataset dataset;
        dataset.table_id = tables[member]->id();
        dataset.database = "partitioned";
        dataset.table = "entities";
        dataset.contract.entity_column = "entity";
        dataset.contract.label_column = "target";
        dataset.contract.feature_columns = params.feature_columns;
        datasets.push_back(dataset);
    }
    std::vector<zetta::ml::SegmentationRunResult> results(3);
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < members.size(); ++index) {
        workers.emplace_back([&, index] {
            zetta::ml::SegmentationRunOptions options;
            options.round_tag = "stored-segmentation";
            results[index] = zetta::ml::run_segmentation(
                members[index]->node, datasets[index], params, options);
        });
    }
    for (auto &worker : workers)
        worker.join();
    for (const auto &result : results) {
        ASSERT_TRUE(result.ok()) << result.error;
        ASSERT_EQ(result.segmentation.metrics.chosen_k, 3);
        ASSERT_EQ(result.segmentation.model.centers.size(),
                  oracle.model.centers.size());
        for (std::size_t cluster = 0; cluster < oracle.model.centers.size();
             ++cluster) {
            for (std::size_t feature = 0;
                 feature < oracle.model.centers[cluster].size(); ++feature)
                EXPECT_NEAR(result.segmentation.model.centers[cluster][feature],
                            oracle.model.centers[cluster][feature], 1e-9);
        }
        if (result.is_root) {
            ASSERT_EQ(result.segmentation.assignments.size(),
                      planted.rows.size());
            std::vector<int> labels(planted.rows.size());
            for (const auto &assignment : result.segmentation.assignments)
                labels[static_cast<std::size_t>(assignment.entity - 1)] =
                    assignment.segment_id;
            EXPECT_GT(zetta::ml::adjusted_rand_index(planted.truth, labels),
                      0.9);
            EXPECT_FALSE(result.run_id.empty());
        }
    }
}
