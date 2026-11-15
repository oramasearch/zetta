// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/label_efficiency.hpp"

namespace zetta {
class Node;
}

namespace zetta::pipeline {
class Run;
}

namespace zetta::ml {

struct LabelEfficiencyRunOptions {
    std::string project;
    std::string graph;
    std::string model_name = "label_efficiency_model";
    LabelEfficiencyParams params;
};

struct LabelEfficiencyRunResult {
    std::shared_ptr<pipeline::Run> run;
    std::string model_id;
    std::string label_queue_table;
    std::string weak_label_table;
    std::string error;

    bool ok() const noexcept {
        return run != nullptr && error.empty();
    }
};

// Run M-19 against a resolved connector-landed table. The distributed trainer
// consumes the node's owned partitions; table materialization is currently
// restricted to a single-member roster so a run never publishes a partial table.
LabelEfficiencyRunResult run_label_efficiency_on_dataset(
    Node& node, const data::ResolvedDataset& dataset,
    const LabelEfficiencyRunOptions& options = {});

class AsyncLabelEfficiencyRun {
public:
    static std::shared_ptr<AsyncLabelEfficiencyRun> start(
        Node& node, data::ResolvedDataset dataset,
        LabelEfficiencyRunOptions options);
    ~AsyncLabelEfficiencyRun();

    AsyncLabelEfficiencyRun(const AsyncLabelEfficiencyRun&) = delete;
    AsyncLabelEfficiencyRun& operator=(const AsyncLabelEfficiencyRun&) = delete;

    bool done() const {
        return done_.load(std::memory_order_acquire);
    }
    const LabelEfficiencyRunResult& result() const {
        return result_;
    }

private:
    AsyncLabelEfficiencyRun(Node& node, data::ResolvedDataset dataset,
                            LabelEfficiencyRunOptions options);

    Node& node_;
    data::ResolvedDataset dataset_;
    LabelEfficiencyRunOptions options_;
    std::atomic<bool> done_{false};
    LabelEfficiencyRunResult result_;
    std::thread worker_;
};

} // namespace zetta::ml
