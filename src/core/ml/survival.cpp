#include "zetta/ml/survival.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr std::chrono::milliseconds kSurvivalTimeout{10000};

double clamp_probability(double value) {
    return std::clamp(value, 1e-12, 1.0 - 1e-12);
}

SurvivalCollectives local_collectives() {
    SurvivalCollectives collectives;
    collectives.allreduce = [](const std::string&, const std::vector<double>& local,
                               GbdtReduce) { return local; };
    collectives.gather = [](const std::string&,
                            const std::vector<double>& local) {
        return std::optional<std::vector<std::vector<double>>>{{local}};
    };
    collectives.broadcast = [](const std::string&,
                               const std::vector<double>& local) {
        return std::optional<std::vector<double>>{local};
    };
    return collectives;
}

std::size_t interval_count(double duration, const SurvivalParams& params) {
    const double bounded = std::max(duration, std::numeric_limits<double>::min());
    const std::size_t intervals = static_cast<std::size_t>(
        std::ceil(bounded / params.interval_width));
    return std::clamp<std::size_t>(intervals, 1, params.max_intervals);
}

SurvivalCurve predict_curve(const SurvivalSubject& subject,
                            const GbdtModel& model,
                            const SurvivalParams& params) {
    SurvivalCurve curve;
    curve.entity = subject.entity;
    curve.points.reserve(params.max_intervals);
    double survival = 1.0;
    double expected = 0.0;
    double median = 0.0;
    for (std::size_t interval = 1; interval <= params.max_intervals;
         ++interval) {
        std::vector<double> row = subject.features;
        row.push_back(static_cast<double>(interval));
        const double hazard = clamp_probability(model.predict_proba(row));
        expected += survival * params.interval_width;
        survival *= 1.0 - hazard;
        const double horizon = params.interval_width *
                               static_cast<double>(interval);
        curve.points.push_back({horizon, hazard, survival});
        if (median == 0.0 && survival <= 0.5) {
            median = horizon;
        }
    }
    curve.expected_time = expected;
    curve.median_survival =
        median == 0.0 ? params.interval_width * params.max_intervals : median;
    return curve;
}

struct EvaluationSubject {
    double duration = 0.0;
    bool event = false;
    bool validation = false;
    SurvivalCurve curve;
};

double concordance(const std::vector<EvaluationSubject>& subjects) {
    double comparable = 0.0;
    double concordant = 0.0;
    for (std::size_t left = 0; left < subjects.size(); ++left) {
        if (!subjects[left].event) {
            continue;
        }
        for (std::size_t right = 0; right < subjects.size(); ++right) {
            if (left == right ||
                subjects[left].duration >= subjects[right].duration) {
                continue;
            }
            comparable += 1.0;
            const double left_time = subjects[left].curve.expected_time;
            const double right_time = subjects[right].curve.expected_time;
            if (left_time < right_time) {
                concordant += 1.0;
            } else if (left_time == right_time) {
                concordant += 0.5;
            }
        }
    }
    return comparable == 0.0 ? 0.5 : concordant / comparable;
}

double censor_survival_before(const std::vector<EvaluationSubject>& subjects,
                              double time) {
    std::vector<double> times;
    times.reserve(subjects.size());
    for (const EvaluationSubject& subject : subjects) {
        if (!subject.event && subject.duration < time) {
            times.push_back(subject.duration);
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());

    double survival = 1.0;
    for (const double censor_time : times) {
        std::uint64_t at_risk = 0;
        std::uint64_t censored = 0;
        for (const EvaluationSubject& subject : subjects) {
            if (subject.duration >= censor_time) {
                ++at_risk;
            }
            if (!subject.event && subject.duration == censor_time) {
                ++censored;
            }
        }
        if (at_risk > 0) {
            survival *= 1.0 - static_cast<double>(censored) /
                                      static_cast<double>(at_risk);
        }
    }
    return std::max(survival, 1e-12);
}

double integrated_brier(const std::vector<EvaluationSubject>& subjects,
                        const SurvivalParams& params) {
    if (subjects.empty()) {
        return 0.0;
    }
    double total = 0.0;
    std::size_t measured = 0;
    for (std::size_t interval = 0; interval < params.max_intervals;
         ++interval) {
        const double horizon = params.interval_width *
                               static_cast<double>(interval + 1);
        double weighted_error = 0.0;
        std::size_t contributions = 0;
        for (const EvaluationSubject& subject : subjects) {
            const double predicted = subject.curve.points[interval].survival;
            if (subject.duration > horizon) {
                const double weight = 1.0 /
                    censor_survival_before(subjects, horizon);
                weighted_error += weight * (1.0 - predicted) *
                                  (1.0 - predicted);
                ++contributions;
            } else if (subject.event) {
                const double weight = 1.0 /
                    censor_survival_before(subjects, subject.duration);
                weighted_error += weight * predicted * predicted;
                ++contributions;
            }
        }
        if (contributions > 0) {
            total += weighted_error / static_cast<double>(subjects.size());
            ++measured;
        }
    }
    return measured == 0 ? 0.0 : total / static_cast<double>(measured);
}

std::vector<IntervalCalibration>
calibration_by_interval(const std::vector<EvaluationSubject>& subjects,
                        const SurvivalParams& params) {
    std::vector<IntervalCalibration> result;
    result.reserve(params.max_intervals);
    for (std::size_t index = 0; index < params.max_intervals; ++index) {
        const double lower = params.interval_width * static_cast<double>(index);
        const double upper = lower + params.interval_width;
        double predicted = 0.0;
        std::uint64_t events = 0;
        std::uint64_t at_risk = 0;
        for (const EvaluationSubject& subject : subjects) {
            if (subject.duration <= lower) {
                continue;
            }
            predicted += subject.curve.points[index].hazard;
            ++at_risk;
            if (subject.event && subject.duration <= upper) {
                ++events;
            }
        }
        result.push_back(
            {index + 1,
             upper,
             at_risk == 0 ? 0.0
                          : predicted / static_cast<double>(at_risk),
             at_risk == 0 ? 0.0
                          : static_cast<double>(events) /
                                static_cast<double>(at_risk),
             at_risk});
    }
    return result;
}

std::vector<double> encode_subjects(
    const std::vector<SurvivalSubject>& subjects,
    const std::vector<SurvivalCurve>& curves, double validation_cutoff,
    const SurvivalParams& params) {
    std::vector<double> wire;
    const std::size_t width = 7 + params.max_intervals;
    wire.reserve(subjects.size() * width);
    for (std::size_t index = 0; index < subjects.size(); ++index) {
        wire.push_back(static_cast<double>(subjects[index].entity));
        wire.push_back(subjects[index].start_time);
        wire.push_back(subjects[index].duration);
        wire.push_back(subjects[index].event ? 1.0 : 0.0);
        wire.push_back(subjects[index].start_time >= validation_cutoff ? 1.0
                                                                      : 0.0);
        wire.push_back(curves[index].expected_time);
        wire.push_back(curves[index].median_survival);
        for (const SurvivalPoint& point : curves[index].points) {
            wire.push_back(point.hazard);
        }
    }
    return wire;
}

std::vector<EvaluationSubject> decode_subjects(
    const std::vector<std::vector<double>>& gathered,
    const SurvivalParams& params) {
    std::vector<EvaluationSubject> result;
    const std::size_t width = 7 + params.max_intervals;
    for (const std::vector<double>& member : gathered) {
        for (std::size_t offset = 0; offset + width <= member.size();
             offset += width) {
            EvaluationSubject subject;
            subject.duration = member[offset + 2];
            subject.event = member[offset + 3] != 0.0;
            subject.validation = member[offset + 4] != 0.0;
            subject.curve.entity = static_cast<std::int64_t>(member[offset]);
            subject.curve.expected_time = member[offset + 5];
            subject.curve.median_survival = member[offset + 6];
            double survival = 1.0;
            for (std::size_t interval = 0; interval < params.max_intervals;
                 ++interval) {
                const double hazard = member[offset + 7 + interval];
                survival *= 1.0 - hazard;
                subject.curve.points.push_back(
                    {params.interval_width * static_cast<double>(interval + 1),
                     hazard, survival});
            }
            result.push_back(std::move(subject));
        }
    }
    return result;
}

std::vector<double> encode_global_curves(
    const std::vector<EvaluationSubject>& subjects,
    const SurvivalParams& params) {
    std::vector<double> wire;
    const std::size_t width = 3 + params.max_intervals;
    wire.reserve(subjects.size() * width);
    for (const EvaluationSubject& subject : subjects) {
        wire.push_back(static_cast<double>(subject.curve.entity));
        wire.push_back(subject.curve.expected_time);
        wire.push_back(subject.curve.median_survival);
        for (const SurvivalPoint& point : subject.curve.points) {
            wire.push_back(point.hazard);
        }
    }
    return wire;
}

std::vector<SurvivalCurve> decode_global_curves(
    const std::vector<double>& wire, const SurvivalParams& params) {
    std::vector<SurvivalCurve> curves;
    const std::size_t width = 3 + params.max_intervals;
    for (std::size_t offset = 0; offset + width <= wire.size();
         offset += width) {
        SurvivalCurve curve;
        curve.entity = static_cast<std::int64_t>(wire[offset]);
        curve.expected_time = wire[offset + 1];
        curve.median_survival = wire[offset + 2];
        double survival = 1.0;
        for (std::size_t interval = 0; interval < params.max_intervals;
             ++interval) {
            const double hazard = wire[offset + 3 + interval];
            survival *= 1.0 - hazard;
            curve.points.push_back(
                {params.interval_width * static_cast<double>(interval + 1),
                 hazard, survival});
        }
        curves.push_back(std::move(curve));
    }
    std::sort(curves.begin(), curves.end(),
              [](const SurvivalCurve& left, const SurvivalCurve& right) {
                  return left.entity < right.entity;
              });
    return curves;
}

std::vector<double> encode_metrics(
    const SurvivalMetrics& metrics,
    const std::vector<IntervalCalibration>& calibration) {
    std::vector<double> wire = {
        metrics.concordance,
        metrics.integrated_brier_score,
        static_cast<double>(metrics.training_subjects),
        static_cast<double>(metrics.validation_subjects),
        static_cast<double>(metrics.event_subjects),
        static_cast<double>(metrics.censored_subjects),
    };
    for (const IntervalCalibration& interval : calibration) {
        wire.push_back(static_cast<double>(interval.interval));
        wire.push_back(interval.horizon);
        wire.push_back(interval.predicted_hazard);
        wire.push_back(interval.observed_event_rate);
        wire.push_back(static_cast<double>(interval.at_risk));
    }
    return wire;
}

bool decode_metrics(const std::vector<double>& wire,
                    const SurvivalParams& params, SurvivalMetrics& metrics,
                    std::vector<IntervalCalibration>& calibration) {
    if (wire.size() != 6 + params.max_intervals * 5) {
        return false;
    }
    metrics.concordance = wire[0];
    metrics.integrated_brier_score = wire[1];
    metrics.training_subjects = static_cast<std::uint64_t>(wire[2]);
    metrics.validation_subjects = static_cast<std::uint64_t>(wire[3]);
    metrics.event_subjects = static_cast<std::uint64_t>(wire[4]);
    metrics.censored_subjects = static_cast<std::uint64_t>(wire[5]);
    calibration.clear();
    for (std::size_t offset = 6; offset < wire.size(); offset += 5) {
        calibration.push_back(
            {static_cast<std::size_t>(wire[offset]), wire[offset + 1],
             wire[offset + 2], wire[offset + 3],
             static_cast<std::uint64_t>(wire[offset + 4])});
    }
    return true;
}

ddf::DataFrameSchema survival_schema(const data::ResolvedDataset& dataset,
                                     const SurvivalDatasetParams& params) {
    using compute::DataType;
    ddf::DataFrameSchema schema;
    schema.columns.push_back(
        {.name = "entity",
         .field = dataset.contract.entity_column,
         .dtype = DataType::Int64,
         .entity_keys = dataset.contract.entity_keys});
    schema.columns.push_back(
        {.name = "start_time",
         .field = dataset.contract.time_column,
         .dtype = DataType::Float64,
         .timestamp = true});
    schema.columns.push_back(
        {.name = "duration",
         .field = params.duration_column,
         .dtype = DataType::Float64});
    schema.columns.push_back(
        {.name = "event",
         .field = dataset.contract.label_column,
         .dtype = DataType::Int64});
    schema.columns.push_back(
        {.name = "feature_time",
         .field = params.feature_time_column,
         .dtype = DataType::Float64,
         .timestamp = true});
    for (const std::string& feature : dataset.contract.feature_columns) {
        schema.columns.push_back(
            {.name = feature, .field = feature, .dtype = DataType::Float64});
    }
    return schema;
}

std::string table_suffix(const SurvivalDatasetParams& params) {
    const std::string& value = params.graph.empty() ? params.project : params.graph;
    return value.empty() ? "pipeline" : value.substr(0, std::min<std::size_t>(8, value.size()));
}

} // namespace

std::vector<PersonPeriodRow>
expand_person_period_rows(const std::vector<SurvivalSubject>& subjects,
                          const SurvivalParams& params) {
    std::vector<PersonPeriodRow> rows;
    if (!(params.interval_width > 0.0) || params.max_intervals == 0) {
        return rows;
    }
    for (const SurvivalSubject& subject : subjects) {
        const std::size_t event_interval = static_cast<std::size_t>(
            std::ceil(subject.duration / params.interval_width));
        const std::size_t count = interval_count(subject.duration, params);
        for (std::size_t interval = 1; interval <= count; ++interval) {
            PersonPeriodRow row;
            row.entity = subject.entity;
            row.interval = interval;
            row.event = subject.event && event_interval <= params.max_intervals &&
                        interval == event_interval;
            row.features = subject.features;
            row.features.push_back(static_cast<double>(interval));
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

SurvivalTrainResult train_survival(
    const std::vector<SurvivalSubject>& local_subjects,
    const std::vector<std::string>& feature_names,
    const SurvivalParams& params,
    const SurvivalCollectives& given_collectives) {
    SurvivalTrainResult result;
    if (!(params.interval_width > 0.0) || params.max_intervals == 0) {
        result.status = compute::ComputeStatus::invalid_argument(
            "survival: interval_width and max_intervals must be positive");
        return result;
    }
    if (!(params.validation_fraction > 0.0 &&
          params.validation_fraction < 1.0)) {
        result.status = compute::ComputeStatus::invalid_argument(
            "survival: validation_fraction must be between 0 and 1");
        return result;
    }
    for (const SurvivalSubject& subject : local_subjects) {
        if (subject.features.size() != feature_names.size()) {
            result.status = compute::ComputeStatus::invalid_argument(
                "survival: subject feature count does not match feature names");
            return result;
        }
        if (!(subject.duration > 0.0) || !std::isfinite(subject.duration) ||
            !std::isfinite(subject.start_time)) {
            result.status = compute::ComputeStatus::invalid_argument(
                "survival: every subject needs a finite positive duration and "
                "finite start time");
            return result;
        }
    }

    SurvivalCollectives collectives = given_collectives;
    const SurvivalCollectives local = local_collectives();
    if (!collectives.allreduce) {
        collectives.allreduce = local.allreduce;
    }
    if (!collectives.gather) {
        collectives.gather = local.gather;
    }
    if (!collectives.broadcast) {
        collectives.broadcast = local.broadcast;
    }

    double local_min = std::numeric_limits<double>::max();
    double local_max = std::numeric_limits<double>::lowest();
    for (const SurvivalSubject& subject : local_subjects) {
        local_min = std::min(local_min, subject.start_time);
        local_max = std::max(local_max, subject.start_time);
    }
    const std::vector<double> global_min = collectives.allreduce(
        "survival.validation.min", {local_min}, GbdtReduce::Min);
    const std::vector<double> global_max = collectives.allreduce(
        "survival.validation.max", {local_max}, GbdtReduce::Max);
    const std::vector<double> global_count = collectives.allreduce(
        "survival.subjects", {static_cast<double>(local_subjects.size())},
        GbdtReduce::Sum);
    if (global_min.size() != 1 || global_max.size() != 1 ||
        global_count.size() != 1 || global_count[0] < 2.0 ||
        !(global_min[0] < global_max[0])) {
        result.status = compute::ComputeStatus::invalid_argument(
            "survival: time-aware validation needs at least two subjects with "
            "different start times");
        return result;
    }
    const double validation_cutoff =
        global_max[0] - params.validation_fraction *
                            (global_max[0] - global_min[0]);

    std::vector<SurvivalSubject> training_subjects;
    for (const SurvivalSubject& subject : local_subjects) {
        if (subject.start_time < validation_cutoff) {
            training_subjects.push_back(subject);
        }
    }
    const std::vector<PersonPeriodRow> rows =
        expand_person_period_rows(training_subjects, params);
    std::vector<std::vector<double>> columns(feature_names.size() + 1);
    std::vector<double> labels;
    labels.reserve(rows.size());
    for (const PersonPeriodRow& row : rows) {
        for (std::size_t feature = 0; feature < row.features.size(); ++feature) {
            columns[feature].push_back(row.features[feature]);
        }
        labels.push_back(row.event ? 1.0 : 0.0);
    }
    std::vector<std::string> period_features = feature_names;
    period_features.push_back("interval");
    const GbdtTrainResult trained = train_gbdt(
        columns, period_features, labels, params.gbdt, collectives.allreduce,
        "survival.gbdt");
    if (!trained.ok()) {
        result.status = trained.status;
        return result;
    }
    result.model = trained.model;
    result.feature_importances = trained.feature_importances;

    std::vector<SurvivalCurve> local_curves;
    local_curves.reserve(local_subjects.size());
    for (const SurvivalSubject& subject : local_subjects) {
        local_curves.push_back(predict_curve(subject, result.model, params));
    }
    const std::optional<std::vector<std::vector<double>>> gathered =
        collectives.gather(
            "survival.predictions",
            encode_subjects(local_subjects, local_curves, validation_cutoff,
                            params));
    if (!gathered) {
        result.status = compute::ComputeStatus::backend_error(
            "survival: prediction gather failed");
        return result;
    }

    std::vector<EvaluationSubject> global_subjects;
    std::vector<double> metrics_wire;
    std::vector<double> curves_wire;
    if (collectives.is_root) {
        global_subjects = decode_subjects(*gathered, params);
        std::vector<EvaluationSubject> validation;
        for (const EvaluationSubject& subject : global_subjects) {
            if (subject.validation) {
                validation.push_back(subject);
            }
            if (subject.event) {
                ++result.metrics.event_subjects;
            } else {
                ++result.metrics.censored_subjects;
            }
            if (subject.validation) {
                ++result.metrics.validation_subjects;
            } else {
                ++result.metrics.training_subjects;
            }
        }
        if (validation.empty() || result.metrics.training_subjects == 0) {
            result.status = compute::ComputeStatus::invalid_argument(
                "survival: time-aware split produced an empty training or "
                "validation set");
            return result;
        }
        result.metrics.concordance = concordance(validation);
        result.metrics.integrated_brier_score =
            integrated_brier(validation, params);
        result.calibration = calibration_by_interval(validation, params);
        metrics_wire = encode_metrics(result.metrics, result.calibration);
        curves_wire = encode_global_curves(global_subjects, params);
    }

    const std::optional<std::vector<double>> broadcast_metrics =
        collectives.broadcast("survival.metrics", metrics_wire);
    const std::optional<std::vector<double>> broadcast_curves =
        collectives.broadcast("survival.curves", curves_wire);
    if (!broadcast_metrics || !broadcast_curves ||
        !decode_metrics(*broadcast_metrics, params, result.metrics,
                        result.calibration)) {
        result.status = compute::ComputeStatus::backend_error(
            "survival: result broadcast failed");
        return result;
    }
    result.curves = decode_global_curves(*broadcast_curves, params);
    result.status = compute::ComputeStatus::success();
    return result;
}

SurvivalRunResult run_survival_on_dataset(
    Node& node, compute::ComputeDevice& device,
    const ir::ExecutionGraph& graph, const data::ResolvedDataset& dataset,
    const SurvivalDatasetParams& params) {
    SurvivalRunResult result;
    node.run_on_owner([&] {
        result.run = node.create_run(params.project, params.graph);
        if (result.run) {
            result.run->set_status(pipeline::RunStatus::Running);
            result.run->add_log({util::now_iso8601(), "info",
                                 "survival training started"});
        }
    });
    const auto fail = [&](std::string reason) {
        result.error = std::move(reason);
        if (result.run) {
            node.run_on_owner([&] {
                result.run->add_log(
                    {util::now_iso8601(), "error", result.error});
                result.run->set_status(pipeline::RunStatus::Failed);
            });
        }
        return result;
    };
    if (!result.run) {
        return fail("survival: could not create the Run record");
    }
    if (!dataset.ok()) {
        return fail(dataset.error);
    }
    if (dataset.contract.entity_column.empty() ||
        dataset.contract.label_column.empty() ||
        dataset.contract.time_column.empty() || params.duration_column.empty() ||
        params.feature_time_column.empty() ||
        dataset.contract.feature_columns.empty()) {
        return fail(
            "survival: entity, event label, start time, duration, feature "
            "timestamp, and at least one feature are required");
    }
    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        return fail("survival: no compute plane is attached to this node");
    }

    ddf::LocalShardResult shard;
    node.run_on_owner([&] {
        shard = ddf::load_local_partitions(
            node, dataset.table_id, survival_schema(dataset, params), device);
    });
    if (!shard.ok()) {
        return fail("survival: load local partitions: " +
                    shard.status.message);
    }

    std::vector<std::int64_t> entities;
    std::vector<std::int64_t> events;
    std::vector<double> starts;
    std::vector<double> durations;
    std::vector<double> feature_times;
    if (!shard.frame.to_host<std::int64_t>("entity", entities).ok() ||
        !shard.frame.to_host<std::int64_t>("event", events).ok() ||
        !shard.frame.to_host<double>("start_time", starts).ok() ||
        !shard.frame.to_host<double>("duration", durations).ok() ||
        !shard.frame.to_host<double>("feature_time", feature_times).ok()) {
        return fail("survival: could not read required columns from the local shard");
    }
    std::vector<std::vector<double>> features(
        dataset.contract.feature_columns.size());
    for (std::size_t column = 0; column < features.size(); ++column) {
        const compute::ComputeStatus status = shard.frame.to_host<double>(
            dataset.contract.feature_columns[column], features[column]);
        if (!status.ok()) {
            return fail("survival: could not read feature '" +
                        dataset.contract.feature_columns[column] + "': " +
                        status.message);
        }
    }

    std::vector<SurvivalSubject> subjects;
    subjects.reserve(entities.size());
    for (std::size_t row = 0; row < entities.size(); ++row) {
        if (feature_times[row] > starts[row]) {
            return fail("survival: entity " + std::to_string(entities[row]) +
                        " has feature data dated after its start cutoff");
        }
        if (events[row] != 0 && events[row] != 1) {
            return fail("survival: event indicator must contain only 0 or 1");
        }
        SurvivalSubject subject;
        subject.entity = entities[row];
        subject.start_time = starts[row];
        subject.duration = durations[row];
        subject.event = events[row] == 1;
        for (const std::vector<double>& feature : features) {
            subject.features.push_back(feature[row]);
        }
        subjects.push_back(std::move(subject));
    }

    std::shared_ptr<compute::ComputeJob> job;
    node.run_on_owner([&] {
        job = node.create_job(compute::JobKind::Train, params.project,
                              params.graph);
    });
    if (!job) {
        return fail("survival: could not create the train job");
    }
    bool is_root = false;
    const compute::JobStatus job_status = coordinator->run(
        *job, [&](compute::JobRound& round) {
            SurvivalCollectives collectives;
            collectives.is_root = round.is_root();
            is_root = collectives.is_root;
            collectives.allreduce = [&](const std::string& tag,
                                        const std::vector<double>& local,
                                        GbdtReduce operation) {
                cluster::CollectiveOp collective = cluster::CollectiveOp::Sum;
                if (operation == GbdtReduce::Min) {
                    collective = cluster::CollectiveOp::Minimum;
                } else if (operation == GbdtReduce::Max) {
                    collective = cluster::CollectiveOp::Maximum;
                }
                const std::optional<std::vector<double>> reduced =
                    round.allreduce(tag, local, collective, kSurvivalTimeout);
                return reduced.value_or(std::vector<double>{});
            };
            collectives.gather = [&](const std::string& tag,
                                     const std::vector<double>& local) {
                return round.gather(tag, local, kSurvivalTimeout);
            };
            collectives.broadcast = [&](const std::string& tag,
                                        const std::vector<double>& local) {
                return round.broadcast(tag, local, kSurvivalTimeout);
            };
            result.training = train_survival(
                subjects, dataset.contract.feature_columns, params.training,
                collectives);
            return compute::JobCoordinator::Outcome{
                result.training.ok(),
                result.training.ok()
                    ? "trained discrete-time hazards for " +
                          std::to_string(result.training.curves.size()) +
                          " subjects"
                    : result.training.status.message};
        });
    if (job_status != compute::JobStatus::Succeeded || !result.training.ok()) {
        return fail("survival train job failed: " + job->detail());
    }

    if (is_root) {
        node.run_on_owner([&] {
            nlohmann::json drivers = nlohmann::json::array();
            const std::size_t driver_count = std::min(
                dataset.contract.feature_columns.size(),
                result.training.feature_importances.size());
            for (std::size_t index = 0; index < driver_count; ++index) {
                drivers.push_back(
                    {{"feature", dataset.contract.feature_columns[index]},
                     {"importance",
                      result.training.feature_importances[index]}});
            }
            ModelValue model_value;
            model_value.name = params.model_name;
            model_value.kind = "survival";
            model_value.project = params.project;
            model_value.run = result.run->id();
            model_value.feature_schema = {
                {"objective", "discrete_time_hazard"},
                {"features", dataset.contract.feature_columns},
                {"drivers", std::move(drivers)}};
            if (dataset.contract.entity_keys) {
                model_value.feature_schema["entity_key_mapping"] =
                    dataset.contract.entity_keys->to_json();
            }
            model_value.metrics = {
                {"concordance", result.training.metrics.concordance},
                {"integrated_brier_score",
                 result.training.metrics.integrated_brier_score},
                {"training_subjects",
                 result.training.metrics.training_subjects},
                {"validation_subjects",
                 result.training.metrics.validation_subjects},
                {"event_subjects", result.training.metrics.event_subjects},
                {"censored_subjects",
                 result.training.metrics.censored_subjects},
                {"interval_width", params.training.interval_width}};
            const std::shared_ptr<Model> model =
                node.create_model(std::move(model_value));
            if (model &&
                model->set_weights(result.training.model.to_json().dump())) {
                result.model_id = model->id();
            }

            std::shared_ptr<data::Database> database =
                node.find_database("survival_results");
            if (!database) {
                database = node.create_database("survival_results");
            }
            if (!database) {
                return;
            }
            const std::string suffix = table_suffix(params);
            const std::string survival_name = "survival_" + suffix;
            const std::string calibration_name = "calibration_" + suffix;
            database->drop_table(survival_name);
            database->drop_table(calibration_name);
            const std::shared_ptr<data::Table> survival_table =
                database->create_table(survival_name);
            const std::shared_ptr<data::Table> calibration_table =
                database->create_table(calibration_name);
            if (survival_table) {
                for (const SurvivalCurve& curve : result.training.curves) {
                    for (const SurvivalPoint& point : curve.points) {
                        survival_table->add_row(
                            {{"entity", data::entity_output(
                                            dataset.contract, curve.entity)},
                             {"horizon", point.horizon},
                             {"hazard", point.hazard},
                             {"survival", point.survival},
                             {"expected_time", curve.expected_time},
                             {"median_survival", curve.median_survival}});
                    }
                }
                result.survival_table =
                    "survival_results." + survival_name;
            }
            if (calibration_table) {
                for (const IntervalCalibration& interval :
                     result.training.calibration) {
                    calibration_table->add_row(
                        {{"interval", interval.interval},
                         {"horizon", interval.horizon},
                         {"predicted_hazard", interval.predicted_hazard},
                         {"observed_event_rate",
                          interval.observed_event_rate},
                         {"at_risk", interval.at_risk}});
                }
                result.calibration_table =
                    "survival_results." + calibration_name;
            }
        });
    }

    node.run_on_owner([&] {
        if (dataset.contract.entity_keys) {
            result.run->set_entity_key_mapping(
                dataset.contract.entity_keys->to_json());
        }
        std::vector<pipeline::RunOutput> outputs;
        if (!result.model_id.empty()) {
            outputs.push_back(
                {"model", "survival model", result.model_id});
        }
        if (!result.survival_table.empty()) {
            outputs.push_back(
                {"dataset", "survival table", result.survival_table});
        }
        if (!result.calibration_table.empty()) {
            outputs.push_back(
                {"dataset", "interval calibration", result.calibration_table});
        }
        outputs.push_back({"report", "run report", result.run->id()});
        result.run->set_outputs(std::move(outputs));
        pipeline::RunLineage lineage;
        lineage.sources = {"dataset:" + dataset.database + "." + dataset.table};
        for (const auto& step : graph.nodes()) {
            lineage.steps.push_back(step->id);
        }
        lineage.freshness = util::now_iso8601();
        lineage.confidence = result.training.metrics.concordance;
        result.run->set_lineage(std::move(lineage));
        for (const std::string& note : params.dataset_notes) {
            result.run->add_log(
                {util::now_iso8601(), "info", "dataset: " + note});
        }
        result.run->add_log(
            {util::now_iso8601(), "info",
             "survival training completed with censoring-aware validation"});
        result.run->set_status(pipeline::RunStatus::Succeeded);
    });
    return result;
}

struct AsyncSurvivalRun::State {
    Node& node;
    ir::ExecutionGraph graph;
    data::ResolvedDataset dataset;
    SurvivalDatasetParams params;
    std::unique_ptr<compute::ComputeDevice> device;
    std::atomic<bool> done{false};
    SurvivalRunResult result;
    std::thread worker;

    State(Node& node_value, ir::ExecutionGraph graph_value,
          data::ResolvedDataset dataset_value,
          SurvivalDatasetParams params_value)
        : node(node_value),
          graph(std::move(graph_value)),
          dataset(std::move(dataset_value)),
          params(std::move(params_value)),
          device(std::make_unique<compute::CpuDevice>()) {}
};

std::shared_ptr<AsyncSurvivalRun> AsyncSurvivalRun::start(
    Node& node, ir::ExecutionGraph graph, data::ResolvedDataset dataset,
    SurvivalDatasetParams params) {
    return std::shared_ptr<AsyncSurvivalRun>(new AsyncSurvivalRun(
        node, std::move(graph), std::move(dataset), std::move(params)));
}

AsyncSurvivalRun::AsyncSurvivalRun(Node& node, ir::ExecutionGraph graph,
                                   data::ResolvedDataset dataset,
                                   SurvivalDatasetParams params)
    : state_(std::make_unique<State>(node, std::move(graph),
                                     std::move(dataset), std::move(params))) {
    State* state = state_.get();
    state->worker = std::thread([state] {
        state->result = run_survival_on_dataset(
            state->node, *state->device, state->graph, state->dataset,
            state->params);
        state->done.store(true, std::memory_order_release);
    });
}

AsyncSurvivalRun::~AsyncSurvivalRun() {
    if (!state_ || !state_->worker.joinable()) {
        return;
    }
    while (!state_->done.load(std::memory_order_acquire)) {
        state_->node.drain_owner_work();
        std::this_thread::yield();
    }
    state_->worker.join();
}

bool AsyncSurvivalRun::done() const {
    return state_->done.load(std::memory_order_acquire);
}

const SurvivalRunResult& AsyncSurvivalRun::result() const {
    return state_->result;
}

} // namespace zetta::ml
