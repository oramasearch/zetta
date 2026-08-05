#include "zetta/agent/corpus_training.hpp"

#include <cstddef>

#include "zetta/agent/softmax_policy.hpp"

namespace zetta::agent {

std::size_t CorpusTrainingReport::improved_count() const {
    std::size_t count = 0;
    for (const TaskTrainingResult& task : tasks) {
        if (task.improved) {
            ++count;
        }
    }
    return count;
}

double CorpusTrainingReport::mean_reward_after() const {
    if (tasks.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const TaskTrainingResult& task : tasks) {
        sum += task.reward_after;
    }
    return sum / static_cast<double>(tasks.size());
}

CorpusTrainingReport train_over_corpus(const std::vector<TaskEnvironment>& corpus,
                                       const UniformSource& uniform,
                                       const TrainingConfig& config) {
    CorpusTrainingReport report;
    report.tasks.reserve(corpus.size());

    for (const TaskEnvironment& environment : corpus) {
        SoftmaxPolicy policy(environment.action_count);
        const TrainingReport training =
            train_softmax_policy(policy, environment.reward, uniform, config);

        report.tasks.push_back(TaskTrainingResult{
            environment.task_id, training.first_reward(), training.last_reward(),
            training.improved()});
    }

    return report;
}

}  // namespace zetta::agent
