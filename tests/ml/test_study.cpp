#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/compute/work_stealing_scheduler.hpp"
#include "zetta/ml/study.hpp"
#include "zetta/node.hpp"

namespace {

zetta::ml::StudySpec synthetic_spec(std::string name,
                                    zetta::ml::SamplerKind sampler,
                                    zetta::ml::PrunerKind pruner,
                                    std::size_t max_trials = 24) {
    zetta::ml::StudySpec spec;
    spec.name = std::move(name);
    spec.methodology = "M-1";
    spec.objective.metric = "validation.score";
    spec.objective.direction = zetta::ml::StudyDirection::Maximize;
    spec.objective.validation_split = "validation";
    zetta::ml::StudyParameter parameter;
    parameter.name = "x";
    parameter.minimum = 0.0;
    parameter.maximum = 1.0;
    spec.search_space = {parameter};
    spec.sampler = sampler;
    spec.pruner = pruner;
    spec.budgets.max_trials = max_trials;
    spec.budgets.max_parallel_trials = 1;
    spec.seed = 1138;
    return spec;
}

double best_value(const zetta::ml::Study& study) {
    double best = -1.0;
    for (const zetta::ml::StudyTrial& trial : study.value().trials) {
        if (trial.objective) best = std::max(best, *trial.objective);
    }
    return best;
}

zetta::ml::TrialOutcome synthetic_objective(
    const nlohmann::json& params, std::uint64_t, const std::string&,
    const zetta::ml::TrialReporter& report, const std::atomic<bool>& stopped) {
    const double x = params.at("x").get<double>();
    const double score = 1.0 - 25.0 * (x - 0.9) * (x - 0.9);
    for (const std::uint64_t step : {1u, 3u, 9u}) {
        const double fraction = static_cast<double>(step) / 9.0;
        if (stopped.load() || !report(step, score * fraction)) {
            return {.complete = false, .pruned = true};
        }
    }
    return {.objective = score, .cost = 1.0};
}

}  // namespace

TEST(Study, TpeWithAshaBeatsRandomAtTheSameTrialBudget) {
    zetta::test::TempDir directory;
    zetta::Node node{{directory.path().string(), false}};
    ASSERT_TRUE(node.start());
    zetta::compute::WorkStealingScheduler scheduler(2);

    const auto random = node.create_study(synthetic_spec(
        "random", zetta::ml::SamplerKind::Random,
        zetta::ml::PrunerKind::Asha));
    const auto tpe = node.create_study(synthetic_spec(
        "tpe", zetta::ml::SamplerKind::Tpe,
        zetta::ml::PrunerKind::Asha));
    ASSERT_TRUE(random);
    ASSERT_TRUE(tpe);

    const auto random_result =
        zetta::ml::run_study(node, *random, scheduler, synthetic_objective);
    const auto tpe_result =
        zetta::ml::run_study(node, *tpe, scheduler, synthetic_objective);

    EXPECT_EQ(random_result.launched, random->spec().budgets.max_trials);
    EXPECT_EQ(tpe_result.launched, tpe->spec().budgets.max_trials);
    EXPECT_GT(best_value(*tpe), best_value(*random))
        << "TPE: " << tpe->metadata().at("trials").dump()
        << "\nrandom: " << random->metadata().at("trials").dump();
    for (const zetta::ml::StudyTrial& trial : tpe->value().trials) {
        EXPECT_TRUE(node.find_run(trial.run));
        EXPECT_EQ(trial.seed, tpe->spec().seed + trial.number);
    }

    const nlohmann::json analysis =
        zetta::ml::StudyOptimizer::analysis(tpe->value());
    EXPECT_FALSE(analysis.at("best_trial").is_null());
    EXPECT_FALSE(analysis.at("optimization_history").empty());
    EXPECT_FALSE(analysis.at("parameter_importances").empty());
    EXPECT_FALSE(analysis.at("parallel_coordinates").empty());
    EXPECT_FALSE(analysis.at("leaderboard").empty());
    node.stop();
}

TEST(Study, AshaPrunesPoorTrialsBeforeTheirLastStep) {
    zetta::test::TempDir directory;
    zetta::Node node{{directory.path().string(), false}};
    ASSERT_TRUE(node.start());
    zetta::compute::WorkStealingScheduler scheduler(2);
    zetta::ml::StudySpec spec = synthetic_spec(
        "pruning", zetta::ml::SamplerKind::Grid,
        zetta::ml::PrunerKind::Asha, 8);
    spec.search_space.front().kind = zetta::ml::ParameterKind::Integer;
    spec.search_space.front().minimum = 0.0;
    spec.search_space.front().maximum = 7.0;
    spec.search_space.front().step = 1.0;
    const auto study = node.create_study(std::move(spec));
    ASSERT_TRUE(study);
    const auto result = zetta::ml::run_study(
        node, *study, scheduler,
        [](const nlohmann::json& params, std::uint64_t, const std::string&,
           const zetta::ml::TrialReporter& report,
           const std::atomic<bool>& stopped) {
            const double score = -params.at("x").get<double>();
            for (const std::uint64_t step : {1u, 3u, 9u}) {
                if (stopped.load() || !report(step, score)) {
                    return zetta::ml::TrialOutcome{.complete = false,
                                                   .pruned = true};
                }
            }
            return zetta::ml::TrialOutcome{.objective = score};
        });
    EXPECT_GT(result.pruned, 0u);
    const auto pruned = std::ranges::find_if(
        study->value().trials, [](const zetta::ml::StudyTrial& trial) {
            return trial.state == zetta::ml::TrialState::Pruned;
        });
    ASSERT_NE(pruned, study->value().trials.end());
    EXPECT_LT(pruned->intermediate.back().step, 9u);
    EXPECT_EQ(node.find_run(pruned->run)->status(),
              zetta::pipeline::RunStatus::Failed);
    node.stop();
}

TEST(Study, RejectsTestSelectionAndPrivateUnreleasedObjectives) {
    std::string error;
    zetta::ml::StudySpec spec = synthetic_spec(
        "invalid", zetta::ml::SamplerKind::Tpe,
        zetta::ml::PrunerKind::Asha);
    spec.objective.validation_split = "test";
    EXPECT_FALSE(zetta::ml::validate_study_spec(spec, &error));
    EXPECT_NE(error.find("non-test"), std::string::npos);

    spec.objective.validation_split = "validation";
    spec.differential_privacy = true;
    EXPECT_FALSE(zetta::ml::validate_study_spec(spec, &error));
    spec.released_metrics = {spec.objective.metric};
    ASSERT_TRUE(zetta::ml::validate_study_spec(spec, &error)) << error;

    zetta::ml::StudyTrial trial;
    trial.run = "private";
    trial.intermediate.push_back({1, -100.0});
    std::vector<zetta::ml::StudyTrial> peers(3);
    for (std::size_t index = 0; index < peers.size(); ++index) {
        peers[index].run = "peer-" + std::to_string(index);
        peers[index].state = zetta::ml::TrialState::Complete;
        peers[index].intermediate.push_back({1, 100.0});
    }
    peers.push_back(trial);
    EXPECT_FALSE(zetta::ml::StudyOptimizer::should_prune(
        spec, peers, peers.back()));
}

TEST(Study, CostAndTrialBudgetsStopAdmissionAndReportTheReason) {
    zetta::test::TempDir directory;
    zetta::Node node{{directory.path().string(), false}};
    ASSERT_TRUE(node.start());
    zetta::compute::WorkStealingScheduler scheduler(2);
    zetta::ml::StudySpec spec = synthetic_spec(
        "cost-cap", zetta::ml::SamplerKind::Random,
        zetta::ml::PrunerKind::None, 10);
    spec.budgets.cost_cap = 2.0;
    const auto study = node.create_study(std::move(spec));
    ASSERT_TRUE(study);
    const auto result = zetta::ml::run_study(
        node, *study, scheduler,
        [](const nlohmann::json&, std::uint64_t, const std::string&,
           const zetta::ml::TrialReporter&,
           const std::atomic<bool>&) {
            return zetta::ml::TrialOutcome{.objective = 1.0, .cost = 1.0};
        });
    EXPECT_EQ(result.launched, 2u);
    EXPECT_EQ(study->value().spent_cost, 2.0);
    EXPECT_EQ(study->status(), zetta::ml::StudyStatus::Stopped);
    EXPECT_EQ(result.stop_reason, "cost cap reached");
    node.stop();
}

TEST(Study, PrivateTrialsSuppressCurvesAndBypassPruning) {
    zetta::test::TempDir directory;
    zetta::Node node{{directory.path().string(), false}};
    ASSERT_TRUE(node.start());
    zetta::compute::WorkStealingScheduler scheduler(2);
    zetta::ml::StudySpec spec = synthetic_spec(
        "private", zetta::ml::SamplerKind::Grid,
        zetta::ml::PrunerKind::Asha, 6);
    spec.differential_privacy = true;
    spec.released_metrics = {spec.objective.metric};
    spec.search_space.front().kind = zetta::ml::ParameterKind::Integer;
    spec.search_space.front().minimum = 0.0;
    spec.search_space.front().maximum = 5.0;
    spec.search_space.front().step = 1.0;
    const auto study = node.create_study(std::move(spec));
    ASSERT_TRUE(study);
    const auto result = zetta::ml::run_study(
        node, *study, scheduler,
        [](const nlohmann::json& params, std::uint64_t, const std::string&,
           const zetta::ml::TrialReporter& report,
           const std::atomic<bool>& stopped) {
            const double score = -params.at("x").get<double>();
            for (const std::uint64_t step : {1u, 3u, 9u}) {
                if (stopped.load() || !report(step, score)) {
                    return zetta::ml::TrialOutcome{.complete = false,
                                                   .pruned = true};
                }
            }
            return zetta::ml::TrialOutcome{.objective = score};
        });
    EXPECT_EQ(result.pruned, 0u);
    EXPECT_EQ(result.completed, 6u);
    for (const zetta::ml::StudyTrial& trial : study->value().trials) {
        const auto run = node.find_run(trial.run);
        ASSERT_TRUE(run);
        EXPECT_EQ(run->metadata().at("metrics_suppressed"),
                  "differential_privacy");
        EXPECT_TRUE(run->get_metrics().at("series").empty());
    }
    node.stop();
}

TEST(Study, WallTimeBudgetCooperativelyStopsAnActiveTrial) {
    zetta::test::TempDir directory;
    zetta::Node node{{directory.path().string(), false}};
    ASSERT_TRUE(node.start());
    zetta::compute::WorkStealingScheduler scheduler(1);
    zetta::ml::StudySpec spec = synthetic_spec(
        "wall-time", zetta::ml::SamplerKind::Random,
        zetta::ml::PrunerKind::None, 5);
    spec.budgets.max_wall_time_ms = 1;
    const auto study = node.create_study(std::move(spec));
    ASSERT_TRUE(study);
    const auto result = zetta::ml::run_study(
        node, *study, scheduler,
        [](const nlohmann::json&, std::uint64_t, const std::string&,
           const zetta::ml::TrialReporter& report,
           const std::atomic<bool>& stopped) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (stopped.load() || !report(1, 0.5)) {
                return zetta::ml::TrialOutcome{.complete = false,
                                               .pruned = true};
            }
            return zetta::ml::TrialOutcome{.objective = 0.5};
        });
    EXPECT_EQ(result.launched, 1u);
    EXPECT_EQ(result.failed, 1u);
    EXPECT_EQ(result.stop_reason, "max wall time reached");
    EXPECT_EQ(study->status(), zetta::ml::StudyStatus::Stopped);
    node.stop();
}

TEST(Study, ReloadsItsRunBackedTrialsFromDisk) {
    zetta::test::TempDir directory;
    std::string study_id;
    std::string run_id;
    {
        zetta::Node node{{directory.path().string(), false}};
        ASSERT_TRUE(node.start());
        const auto study = node.create_study(synthetic_spec(
            "durable", zetta::ml::SamplerKind::Random,
            zetta::ml::PrunerKind::None, 1));
        const auto run = node.create_run("", "durable-trial");
        ASSERT_TRUE(study);
        ASSERT_TRUE(run);
        study_id = study->id();
        run_id = run->id();
        zetta::ml::StudyTrial trial;
        trial.run = run_id;
        trial.seed = 1138;
        ASSERT_TRUE(study->add_trial(std::move(trial)));
        ASSERT_TRUE(study->finish_trial(run_id,
                                        zetta::ml::TrialState::Complete,
                                        0.8, 0.5));
        node.stop();
    }
    zetta::Node reloaded{{directory.path().string(), false}};
    ASSERT_TRUE(reloaded.start());
    const auto study = reloaded.find_study(study_id);
    ASSERT_TRUE(study);
    ASSERT_EQ(study->value().trials.size(), 1u);
    EXPECT_EQ(study->value().trials.front().run, run_id);
    EXPECT_EQ(study->value().trials.front().seed, 1138u);
    EXPECT_TRUE(reloaded.find_run(run_id));
    reloaded.stop();
}
