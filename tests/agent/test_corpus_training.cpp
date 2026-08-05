#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "zetta/agent/corpus_training.hpp"
#include "zetta/agent/training.hpp"

namespace {

using zetta::agent::CorpusTrainingReport;
using zetta::agent::TaskEnvironment;
using zetta::agent::train_over_corpus;
using zetta::agent::TrainingConfig;

// A bandit environment whose `target` arm rewards, out of `arms`.
TaskEnvironment bandit(std::string id, std::size_t arms, std::size_t target) {
    return TaskEnvironment{std::move(id), arms,
                           [target](std::size_t action) {
                               return action == target ? 1.0 : 0.0;
                           }};
}

TEST(CorpusTraining, EachTaskImprovesAndIsReported) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    const std::vector<TaskEnvironment> corpus = {
        bandit("task-a", 3, 0), bandit("task-b", 4, 2), bandit("task-c", 2, 1)};

    const CorpusTrainingReport report = train_over_corpus(
        corpus, [&] { return dist(rng); }, TrainingConfig{8, 150, 0.5});

    ASSERT_EQ(report.tasks.size(), 3u);
    EXPECT_EQ(report.improved_count(), 3u);
    EXPECT_GT(report.mean_reward_after(), 0.8);
    // The report preserves task identity and shows the before/after climb.
    EXPECT_EQ(report.tasks[0].task_id, "task-a");
    for (const auto& task : report.tasks) {
        EXPECT_TRUE(task.improved);
        EXPECT_GT(task.reward_after, task.reward_before);
    }
}

TEST(CorpusTraining, EmptyCorpusIsEmptyReport) {
    const CorpusTrainingReport report =
        train_over_corpus({}, [] { return 0.0; });
    EXPECT_TRUE(report.tasks.empty());
    EXPECT_EQ(report.improved_count(), 0u);
    EXPECT_DOUBLE_EQ(report.mean_reward_after(), 0.0);
}

}  // namespace
