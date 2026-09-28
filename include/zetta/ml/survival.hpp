#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zetta/compute/compute_status.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/gbdt.hpp"

namespace zetta {
class Node;
}

namespace zetta::compute {
class ComputeDevice;
class ComputeJob;
class JobCoordinator;
}

namespace zetta::pipeline {
class Run;
}

// Native discrete-time survival analysis (#1044). Subjects are expanded into
// person-period rows, a distributed histogram GBDT learns the conditional event
// hazard, and interval hazards are converted to survival probabilities.
//
// See docs/ml/survival.md.
namespace zetta::ml {

struct SurvivalParams {
    double interval_width = 30.0;
    std::size_t max_intervals = 12;
    double validation_fraction = 0.2;
    std::uint64_t seed = 0;
    GbdtParams gbdt;
};

struct SurvivalSubject {
    std::int64_t entity = 0;
    double start_time = 0.0;
    double duration = 0.0;
    bool event = false;
    std::vector<double> features;
};

struct PersonPeriodRow {
    std::int64_t entity = 0;
    std::size_t interval = 0;
    bool event = false;
    std::vector<double> features;
};

struct SurvivalPoint {
    double horizon = 0.0;
    double hazard = 0.0;
    double survival = 1.0;
};

struct SurvivalCurve {
    std::int64_t entity = 0;
    double expected_time = 0.0;
    double median_survival = 0.0;
    std::vector<SurvivalPoint> points;
};

struct IntervalCalibration {
    std::size_t interval = 0;
    double horizon = 0.0;
    double predicted_hazard = 0.0;
    double observed_event_rate = 0.0;
    std::uint64_t at_risk = 0;
};

struct SurvivalMetrics {
    double concordance = 0.5;
    double integrated_brier_score = 0.0;
    std::uint64_t training_subjects = 0;
    std::uint64_t validation_subjects = 0;
    std::uint64_t event_subjects = 0;
    std::uint64_t censored_subjects = 0;
};

using SurvivalGather = std::function<
    std::optional<std::vector<std::vector<double>>>(
        const std::string&, const std::vector<double>&)>;
using SurvivalBroadcast = std::function<std::optional<std::vector<double>>(
    const std::string&, const std::vector<double>&)>;

struct SurvivalCollectives {
    AllReduce allreduce;
    SurvivalGather gather;
    SurvivalBroadcast broadcast;
    bool is_root = true;
};

struct SurvivalTrainResult {
    GbdtModel model;
    std::vector<double> feature_importances;
    std::vector<SurvivalCurve> curves;
    std::vector<IntervalCalibration> calibration;
    SurvivalMetrics metrics;
    compute::ComputeStatus status;

    bool ok() const noexcept { return status.ok(); }
};

std::vector<PersonPeriodRow>
expand_person_period_rows(const std::vector<SurvivalSubject>& subjects,
                          const SurvivalParams& params);

SurvivalTrainResult train_survival(
    const std::vector<SurvivalSubject>& local_subjects,
    const std::vector<std::string>& feature_names,
    const SurvivalParams& params,
    const SurvivalCollectives& collectives = {});

struct SurvivalDatasetParams {
    SurvivalParams training;
    std::string duration_column = "duration";
    std::string feature_time_column = "feature_time";
    std::string model_name = "survival_model";
    std::string project;
    std::string graph;
};

struct SurvivalRunResult {
    std::shared_ptr<pipeline::Run> run;
    std::string model_id;
    std::string survival_table;
    std::string calibration_table;
    SurvivalTrainResult training;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

// Run over a resolved partitioned table. Every participating member calls this
// function over its owned partitions; training statistics combine through the
// node's live JobCoordinator collectives. Feature timestamps later than their
// subject start are refused before any model is fit.
SurvivalRunResult run_survival_on_dataset(
    Node& node, compute::ComputeDevice& device,
    const ir::ExecutionGraph& graph, const data::ResolvedDataset& dataset,
    const SurvivalDatasetParams& params = {});

class AsyncSurvivalRun {
public:
    static std::shared_ptr<AsyncSurvivalRun> start(
        Node& node, ir::ExecutionGraph graph, data::ResolvedDataset dataset,
        SurvivalDatasetParams params);
    ~AsyncSurvivalRun();

    AsyncSurvivalRun(const AsyncSurvivalRun&) = delete;
    AsyncSurvivalRun& operator=(const AsyncSurvivalRun&) = delete;

    bool done() const;
    const SurvivalRunResult& result() const;

private:
    AsyncSurvivalRun(Node& node, ir::ExecutionGraph graph,
                     data::ResolvedDataset dataset,
                     SurvivalDatasetParams params);

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace zetta::ml
