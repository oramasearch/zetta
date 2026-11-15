// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <string>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/ensemble.hpp"
#include "zetta/pipeline/executor.hpp"

namespace zetta {
class Node;
}

namespace zetta::compute {
class ComputeDevice;
}

// The persisted M-18 runner. It consumes the same ResolvedDataset contract used
// by table-backed churn runs, drives the native distributed trainer, registers
// the ensemble Model, and writes blended-score and meta-weight tables.
namespace zetta::ml {

struct Methodology;

struct EnsembleRunOptions {
    EnsembleParams params;
    std::string project;
    std::string graph;
    std::string model_name = "ensemble_model";
};

struct EnsembleRunResult {
    pipeline::ExecutionResult execution;
    std::string model_id;
    std::string scores_table;
    std::string weights_table;

    bool ok() const {
        return execution.ok;
    }
};

EnsembleRunResult run_ensemble_on_dataset(
    Node& node, compute::ComputeDevice& device,
    const ir::ExecutionGraph& graph, const data::ResolvedDataset& dataset,
    const EnsembleRunOptions& options = {});

Methodology make_m18_methodology();

}  // namespace zetta::ml
