#include "zetta/ddf/featurize_job.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/join.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/storage/json_store.hpp"

namespace zetta::ddf {

namespace {

using compute::ComputeStatus;
using compute::DataType;
using compute::JobRound;

// A deterministic, spec-derived collective tag — the default round_tag. Same spec ->
// same tag on every node, so SPMD callers meet in the same rounds without agreeing
// one explicitly. (Mirrors group_by_job's default_round_tag.)
std::string default_round_tag(const FeaturizeSpec& spec) {
    std::string tag = "ddf.featurize:" + spec.entity_column + "/" + spec.time_column;
    for (const WindowedFeature& feature : spec.event_features) {
        tag += ":";
        tag += feature.name;
    }
    return tag;
}

// Materialize a JoinedRows (the distributed_join output) back into a DataFrame on
// `device` so the windowed group-bys can run over it. Every column is Float64 — the
// entity key rides as an exact-integer double (<= 2^53), the same precision bound the
// whole double-based collective plane carries.
DataFrameResult joined_to_frame(const JoinedRows& joined, compute::ComputeDevice& device) {
    JsonStore store;
    const std::size_t width = joined.width();
    for (std::size_t row = 0; row < joined.row_count(); ++row) {
        nlohmann::json doc;
        for (std::size_t column = 0; column < width; ++column) {
            doc[joined.columns[column]] = joined.data[row * width + column];
        }
        store.add("r" + std::to_string(row), std::move(doc));
    }
    DataFrameSchema schema;
    for (const std::string& name : joined.columns) {
        schema.columns.push_back(ColumnSchema{.name = name, .dtype = DataType::Float64});
    }
    return load_data_frame(store, schema, device);
}

// Filter `frame` to a feature's window: time <= as_of, optional time >= window_start,
// optional type_column == type_value. Each filter runs the DDF filter op (gather).
DataFrameResult window_filter(const DataFrame& frame, const FeaturizeSpec& spec,
                              const WindowedFeature& feature) {
    DataFrame storage;
    const DataFrame* current = &frame;
    const auto apply = [&](const FilterPredicate& predicate) -> ComputeStatus {
        DataFrameResult result = filter(*current, predicate);
        if (!result.ok()) return result.status;
        storage = std::move(result.frame);
        current = &storage;
        return ComputeStatus::success();
    };

    ComputeStatus status = apply(FilterPredicate{.column = spec.time_column,
                                                 .comparison = Comparison::LessEqual,
                                                 .value = spec.as_of});
    if (!status.ok()) return {DataFrame{}, status};
    if (feature.window_start.has_value()) {
        status = apply(FilterPredicate{.column = spec.time_column,
                                       .comparison = Comparison::GreaterEqual,
                                       .value = *feature.window_start});
        if (!status.ok()) return {DataFrame{}, status};
    }
    if (feature.has_type_filter()) {
        status = apply(FilterPredicate{.column = feature.type_column,
                                       .comparison = Comparison::Equal,
                                       .value = *feature.type_value});
        if (!status.ok()) return {DataFrame{}, status};
    }
    return {std::move(storage), ComputeStatus::success()};
}

}  // namespace

DistributedFeaturizeResult run_featurize_job(compute::JobCoordinator& coordinator,
                                             ComputeJob& job, const DataFrame& events,
                                             const DataFrame& accounts,
                                             const FeaturizeSpec& spec,
                                             const std::string& round_tag,
                                             std::chrono::milliseconds timeout) {
    DistributedFeaturizeResult out;
    const std::string tag = round_tag.empty() ? default_round_tag(spec) : round_tag;

    using Outcome = compute::JobCoordinator::Outcome;
    out.status = coordinator.run(job, [&](JobRound& round) -> Outcome {
        out.is_root = round.is_root();
        compute::ComputeDevice* device = events.device() != nullptr ? events.device()
                                                                    : accounts.device();
        if (device == nullptr) {
            return {.ok = false, .detail = "featurize: input frames have no device"};
        }

        // 1. Repartition + enrich: join the events shard with the accounts shard on
        // the entity key. After the join every event of an entity, carrying that
        // entity's static account columns, is co-located on one node.
        DistributedJoinResult enriched = distributed_join(
            round, events, spec.entity_column, accounts, spec.entity_column,
            JoinAlgorithm::Hash, tag + ".join");
        if (!enriched.ok()) {
            return {.ok = false, .detail = "featurize join: " + enriched.status.message};
        }
        DataFrameResult enriched_frame = joined_to_frame(enriched.rows, *device);
        if (!enriched_frame.ok()) {
            return {.ok = false,
                    .detail = "featurize materialize: " + enriched_frame.status.message};
        }

        // 2a. The static account dimension (+ label): constant per entity, so a Min
        // over each entity's enriched rows returns the constant. A leading Count
        // gives the entity universe + keeps the spec valid when there are no static
        // columns. distributed_group_by broadcasts the merged result to every node.
        GroupBySpec dim_spec;
        dim_spec.key = spec.entity_column;
        dim_spec.aggregates.push_back(Aggregate{.op = AggregateOp::Count,
                                                .column = {},
                                                .output = "__rows"});
        for (const std::string& column : spec.account_features) {
            dim_spec.aggregates.push_back(
                Aggregate{.op = AggregateOp::Min, .column = column, .output = column});
        }
        if (!spec.label_column.empty()) {
            dim_spec.aggregates.push_back(Aggregate{
                .op = AggregateOp::Min, .column = spec.label_column, .output = spec.label_column});
        }
        GroupByOutcome dimension = distributed_group_by(enriched_frame.frame, dim_spec,
                                                        round, tag + ".dim", timeout);
        if (!dimension.ok()) {
            return {.ok = false, .detail = "featurize dim: " + dimension.status.message};
        }

        // The entity universe (ascending keys of the dimension group-by).
        const std::vector<double>& universe = dimension.result.keys;

        // 2b. Each windowed event feature: window-filter the enriched rows, then a
        // distributed group-by per entity. Densify to the universe with 0-fill.
        std::vector<std::vector<double>> feature_columns(spec.event_features.size());
        for (std::size_t index = 0; index < spec.event_features.size(); ++index) {
            const WindowedFeature& feature = spec.event_features[index];
            DataFrameResult windowed = window_filter(enriched_frame.frame, spec, feature);
            if (!windowed.ok()) {
                return {.ok = false,
                        .detail = "featurize filter '" + feature.name + "': " +
                                  windowed.status.message};
            }
            GroupBySpec feature_spec;
            feature_spec.key = spec.entity_column;
            feature_spec.aggregates = {Aggregate{
                .op = feature.agg, .column = feature.value_column, .output = feature.name}};
            GroupByOutcome aggregated = distributed_group_by(
                windowed.frame, feature_spec, round,
                tag + ".feat." + std::to_string(index), timeout);
            if (!aggregated.ok()) {
                return {.ok = false,
                        .detail = "featurize feature '" + feature.name + "': " +
                                  aggregated.status.message};
            }
            // Map the (subset) group keys to values, then densify to the universe.
            std::map<double, double> by_key;
            const std::vector<double>* values = aggregated.result.value(feature.name);
            for (std::size_t group = 0; values != nullptr &&
                                        group < aggregated.result.keys.size();
                 ++group) {
                by_key[aggregated.result.keys[group]] = (*values)[group];
            }
            feature_columns[index].reserve(universe.size());
            for (const double key : universe) {
                const auto found = by_key.find(key);
                feature_columns[index].push_back(found == by_key.end() ? 0.0
                                                                       : found->second);
            }
        }

        // 3. Assemble the global feature table (identical on every node).
        FeatureTable& table = out.table;
        table = FeatureTable{};
        for (const double key : universe) {
            table.entities.push_back(static_cast<std::int64_t>(key));
        }
        table.feature_names = spec.account_features;
        for (const WindowedFeature& feature : spec.event_features) {
            table.feature_names.push_back(feature.name);
        }
        table.features.resize(table.feature_names.size());
        for (std::size_t column = 0; column < spec.account_features.size(); ++column) {
            const std::vector<double>* values =
                dimension.result.value(spec.account_features[column]);
            table.features[column] = values != nullptr ? *values
                                                       : std::vector<double>(universe.size(), 0.0);
        }
        for (std::size_t index = 0; index < spec.event_features.size(); ++index) {
            table.features[spec.account_features.size() + index] =
                std::move(feature_columns[index]);
        }
        if (!spec.label_column.empty()) {
            table.label_name = spec.label_column;
            const std::vector<double>* values = dimension.result.value(spec.label_column);
            table.labels = values != nullptr ? *values
                                             : std::vector<double>(universe.size(), 0.0);
        }

        return {.ok = true,
                .detail = "featurize over " +
                          std::to_string(round.participants().size()) + " nodes -> " +
                          std::to_string(table.row_count()) + " entities x " +
                          std::to_string(table.feature_count()) + " features"};
    });

    return out;
}

}  // namespace zetta::ddf
