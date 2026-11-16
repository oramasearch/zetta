// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/survival.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

std::vector<zetta::ml::SurvivalSubject> synthetic_subjects() {
    std::vector<zetta::ml::SurvivalSubject> subjects;
    for (std::int64_t entity = 0; entity < 240; ++entity) {
        const bool high_hazard = entity % 2 == 0;
        const bool censored = entity % 7 == 0;
        const double jitter = static_cast<double>((entity * 17) % 20);
        subjects.push_back(
            {.entity = entity,
             .start_time = static_cast<double>(entity),
             .duration = high_hazard ? 35.0 + jitter : 190.0 + jitter,
             .event = !censored,
             .features = {high_hazard ? 1.0 : 0.0}});
    }
    return subjects;
}

zetta::ml::SurvivalParams survival_params() {
    zetta::ml::SurvivalParams params;
    params.interval_width = 30.0;
    params.max_intervals = 10;
    params.validation_fraction = 0.2;
    params.seed = 17;
    params.gbdt.num_trees = 24;
    params.gbdt.max_depth = 3;
    params.gbdt.max_bins = 16;
    params.gbdt.learning_rate = 0.2;
    return params;
}

zetta::cluster::ClusterConfig cluster_config(
    std::uint16_t port, const std::vector<std::string>& seeds) {
    zetta::cluster::ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.port_range = 1;
    config.seeds = seeds;
    return config;
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

struct SurvivalMember {
    SurvivalMember(std::string id, std::uint16_t port,
                   std::vector<std::string> seeds)
        : node(zetta::NodeConfig{directory.path().string(), false}),
          cluster(std::move(id), cluster_config(port, seeds)),
          coordinator(cluster, 2) {
        node.start();
        cluster.start();
        coordinator.start();
    }

    ~SurvivalMember() {
        coordinator.stop();
        cluster.stop();
        node.stop();
    }

    zetta::test::TempDir directory;
    zetta::Node node;
    zetta::cluster::Cluster cluster;
    zetta::compute::JobCoordinator coordinator;
};

} // namespace

TEST(SurvivalHazard, CensoredSubjectsNeverProduceAnEventLabel) {
    const zetta::ml::SurvivalParams params = survival_params();
    const std::vector<zetta::ml::PersonPeriodRow> rows =
        zetta::ml::expand_person_period_rows(
            {{.entity = 1,
              .start_time = 0.0,
              .duration = 75.0,
              .event = false,
              .features = {1.0}},
             {.entity = 2,
              .start_time = 0.0,
              .duration = 75.0,
              .event = true,
              .features = {1.0}}},
            params);

    std::size_t censored_events = 0;
    std::size_t observed_events = 0;
    for (const zetta::ml::PersonPeriodRow& row : rows) {
        if (row.entity == 1 && row.event) {
            ++censored_events;
        }
        if (row.entity == 2 && row.event) {
            ++observed_events;
        }
    }
    EXPECT_EQ(censored_events, 0u);
    EXPECT_EQ(observed_events, 1u);
}

TEST(SurvivalHazard, LearnsKnownHazardAndProducesMonotoneCurves) {
    const zetta::ml::SurvivalTrainResult result = zetta::ml::train_survival(
        synthetic_subjects(), {"risk_feature"}, survival_params());
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_GT(result.metrics.concordance, 0.75);
    ASSERT_EQ(result.feature_importances.size(), 2u);
    EXPECT_GT(result.feature_importances[0], 0.0);

    for (const zetta::ml::SurvivalCurve& curve : result.curves) {
        double previous = 1.0;
        for (const zetta::ml::SurvivalPoint& point : curve.points) {
            EXPECT_LE(point.survival, previous);
            EXPECT_GE(point.survival, 0.0);
            EXPECT_LE(point.survival, 1.0);
            previous = point.survival;
        }
    }

    const auto high = std::find_if(
        result.curves.begin(), result.curves.end(),
        [](const zetta::ml::SurvivalCurve& curve) { return curve.entity == 200; });
    const auto low = std::find_if(
        result.curves.begin(), result.curves.end(),
        [](const zetta::ml::SurvivalCurve& curve) { return curve.entity == 201; });
    ASSERT_NE(high, result.curves.end());
    ASSERT_NE(low, result.curves.end());
    EXPECT_LT(high->expected_time, low->expected_time);
}

TEST(SurvivalConcordance, IsDeterministicUnderASeed) {
    const zetta::ml::SurvivalTrainResult first = zetta::ml::train_survival(
        synthetic_subjects(), {"risk_feature"}, survival_params());
    const zetta::ml::SurvivalTrainResult second = zetta::ml::train_survival(
        synthetic_subjects(), {"risk_feature"}, survival_params());
    ASSERT_TRUE(first.ok()) << first.status.message;
    ASSERT_TRUE(second.ok()) << second.status.message;
    EXPECT_EQ(first.model.to_json(), second.model.to_json());
    EXPECT_DOUBLE_EQ(first.metrics.concordance, second.metrics.concordance);
    EXPECT_DOUBLE_EQ(first.metrics.integrated_brier_score,
                     second.metrics.integrated_brier_score);
}

TEST(SurvivalHazardPartitions, DistributedTrainingMatchesSingleNode) {
    const std::vector<zetta::ml::SurvivalSubject> subjects =
        synthetic_subjects();
    const zetta::ml::SurvivalTrainResult oracle = zetta::ml::train_survival(
        subjects, {"risk_feature"}, survival_params());
    ASSERT_TRUE(oracle.ok()) << oracle.status.message;

    SurvivalMember first("survival-member-a", 24840, {});
    SurvivalMember second("survival-member-b", 24841,
                          {"127.0.0.1:24840"});
    ASSERT_TRUE(wait_for([&] {
        return first.cluster.peer_count() == 1 &&
               second.cluster.peer_count() == 1;
    }));

    std::vector<zetta::ml::SurvivalSubject> shards[2];
    for (const zetta::ml::SurvivalSubject& subject : subjects) {
        shards[static_cast<std::size_t>(subject.entity) % 2].push_back(subject);
    }
    zetta::ml::SurvivalTrainResult results[2];
    std::thread workers[2];
    SurvivalMember* members[2] = {&first, &second};
    for (std::size_t index = 0; index < 2; ++index) {
        workers[index] = std::thread([&, index] {
            const std::shared_ptr<zetta::compute::ComputeJob> job =
                members[index]->node.create_job(
                    zetta::compute::JobKind::Train);
            ASSERT_TRUE(job);
            members[index]->coordinator.run(
                *job, [&](zetta::compute::JobRound& round) {
                    zetta::ml::SurvivalCollectives collectives;
                    collectives.is_root = round.is_root();
                    collectives.allreduce =
                        [&](const std::string& tag,
                            const std::vector<double>& local,
                            zetta::ml::GbdtReduce operation) {
                            zetta::cluster::CollectiveOp collective =
                                zetta::cluster::CollectiveOp::Sum;
                            if (operation == zetta::ml::GbdtReduce::Min) {
                                collective =
                                    zetta::cluster::CollectiveOp::Minimum;
                            } else if (operation ==
                                       zetta::ml::GbdtReduce::Max) {
                                collective =
                                    zetta::cluster::CollectiveOp::Maximum;
                            }
                            return round
                                .allreduce(tag, local, collective, 8s)
                                .value_or(std::vector<double>{});
                        };
                    collectives.gather =
                        [&](const std::string& tag,
                            const std::vector<double>& local) {
                            return round.gather(tag, local, 8s);
                        };
                    collectives.broadcast =
                        [&](const std::string& tag,
                            const std::vector<double>& local) {
                            return round.broadcast(tag, local, 8s);
                        };
                    results[index] = zetta::ml::train_survival(
                        shards[index], {"risk_feature"}, survival_params(),
                        collectives);
                    return zetta::compute::JobCoordinator::Outcome{
                        results[index].ok(), results[index].status.message};
                });
        });
    }
    workers[0].join();
    workers[1].join();

    for (const zetta::ml::SurvivalTrainResult& result : results) {
        ASSERT_TRUE(result.ok()) << result.status.message;
        ASSERT_EQ(result.curves.size(), oracle.curves.size());
        for (std::size_t curve = 0; curve < result.curves.size(); ++curve) {
            EXPECT_EQ(result.curves[curve].entity,
                      oracle.curves[curve].entity);
            EXPECT_NEAR(result.curves[curve].expected_time,
                        oracle.curves[curve].expected_time, 1e-10);
            ASSERT_EQ(result.curves[curve].points.size(),
                      oracle.curves[curve].points.size());
            for (std::size_t point = 0;
                 point < result.curves[curve].points.size(); ++point) {
                EXPECT_NEAR(result.curves[curve].points[point].hazard,
                            oracle.curves[curve].points[point].hazard, 1e-10);
            }
        }
        EXPECT_NEAR(result.metrics.concordance,
                    oracle.metrics.concordance, 1e-12);
        EXPECT_NEAR(result.metrics.integrated_brier_score,
                    oracle.metrics.integrated_brier_score, 1e-12);
    }
}
