// SPDX-License-Identifier: MIT

#include "zetta/ml/segmentation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

using compute::JobRound;
using Outcome = compute::JobCoordinator::Outcome;

zetta::cluster::CollectiveOp
collective_operation(SegmentationReduce operation) {
    switch (operation) {
    case SegmentationReduce::Sum:
        return zetta::cluster::CollectiveOp::Sum;
    case SegmentationReduce::Minimum:
        return zetta::cluster::CollectiveOp::Minimum;
    case SegmentationReduce::Maximum:
        return zetta::cluster::CollectiveOp::Maximum;
    }
    return zetta::cluster::CollectiveOp::Sum;
}

std::optional<std::string> read_blob(Node &node, const std::string &reference) {
    for (const std::string &database_name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(database_name);
        if (!database)
            continue;
        if (const std::optional<std::string> body =
                database->large_objects().read(reference)) {
            return body;
        }
    }
    return std::nullopt;
}

bool append_row(const nlohmann::json &document,
                const zetta::data::DatasetContract &contract,
                const SegmentationParams &params,
                std::vector<SegmentationRow> &rows, std::string &error) {
    if (!document.is_object()) {
        error = "segmentation dataset: a row is not an object";
        return false;
    }
    const auto entity = document.find(contract.entity_column);
    if (entity == document.end()) {
        error = "segmentation dataset: entity column '" +
                contract.entity_column + "' is missing";
        return false;
    }
    std::int64_t entity_key = 0;
    if (contract.entity_keys && entity->is_string()) {
        const std::optional<std::int64_t> derived =
            contract.entity_keys->derived(
                entity->get_ref<const std::string&>());
        if (!derived) {
            error = "segmentation dataset: text entity is absent from the "
                    "resolved mapping";
            return false;
        }
        entity_key = *derived;
    } else if (entity->is_number_unsigned() &&
        entity->get<std::uint64_t>() <=
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
        entity_key = static_cast<std::int64_t>(entity->get<std::uint64_t>());
    } else if (entity->is_number_integer() && !entity->is_number_unsigned()) {
        entity_key = entity->get<std::int64_t>();
    } else if (entity->is_number_float() &&
               std::isfinite(entity->get<double>()) &&
               entity->get<double>() >= -0x1p63 &&
               entity->get<double>() < 0x1p63 &&
               std::trunc(entity->get<double>()) == entity->get<double>()) {
        entity_key = static_cast<std::int64_t>(entity->get<double>());
    } else {
        error = "segmentation dataset: entity column '" +
                contract.entity_column + "' is not integer-valued";
        return false;
    }
    if (!contract.time_column.empty()) {
        const auto time = document.find(contract.time_column);
        const std::optional<double> time_seconds =
            time == document.end()
                ? std::nullopt
                : zetta::util::parse_time_seconds(*time);
        if (!time_seconds || !std::isfinite(contract.as_of)) {
            error = "segmentation dataset: invalid time cutoff or time value";
            return false;
        }
        if (*time_seconds > contract.as_of)
            return true;
    }
    SegmentationRow row;
    row.entity = entity_key;
    row.values = nlohmann::json::object();
    // Copy only approved features. The label is never looked up, so an invalid
    // or adversarial target value cannot influence clustering or even fail
    // parsing.
    for (const std::string &feature : contract.feature_columns) {
        if (segmentation_excludes_column(feature, params) ||
            feature == contract.time_column)
            continue;
        const auto value = document.find(feature);
        if (value == document.end()) {
            error = "segmentation dataset: feature column '" + feature +
                    "' is missing";
            return false;
        }
        row.values[feature] = *value;
    }
    rows.push_back(std::move(row));
    return true;
}

bool load_owned_rows(Node &node, const zetta::data::ResolvedDataset &dataset,
                     const SegmentationParams &params, bool is_root,
                     std::uint64_t &read_count, std::uint64_t &expected_count,
                     std::vector<SegmentationRow> &rows, std::string &error) {
    const auto database = node.find_database(dataset.database);
    const auto table = database ? database->find_table(dataset.table) : nullptr;
    if (!table || table->id() != dataset.table_id) {
        error = "segmentation dataset: resolved table is absent or has changed";
        return false;
    }
    std::uint64_t visible = 0;
    std::vector<std::shared_ptr<zetta::data::DatasetPartition>> owned;
    for (const std::shared_ptr<zetta::data::DatasetPartition> &partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != dataset.table_id)
            continue;
        ++visible;
        expected_count +=
            partition->row_range().end - partition->row_range().start;
        if (partition->owner_nodes().empty()) {
            error = "segmentation dataset: partition has no owner";
            return false;
        }
        // Exactly one replica contributes. An absent designated owner fails the
        // collective row-count check; replicas must never double the
        // population.
        const auto primary = std::min_element(partition->owner_nodes().begin(),
                                              partition->owner_nodes().end());
        if (*primary != node.node_id()) {
            continue;
        }
        owned.push_back(partition);
    }
    std::sort(owned.begin(), owned.end(),
              [](const auto &left, const auto &right) {
                  return left->row_range().start < right->row_range().start;
              });
    if (table->partition_manifest()) {
        error = zetta::data::catalog_shortfall(*table->partition_manifest(),
                                               visible);
        if (!error.empty())
            return false;
    }
    if (table->partition_only() && !table->partition_manifest()) {
        error = "segmentation dataset: partition manifest is not available";
        return false;
    }
    if (visible > 0 || table->partition_manifest()) {
        for (const std::shared_ptr<zetta::data::DatasetPartition> &partition :
             owned) {
            if (partition->blob_ref().empty()) {
                error = "segmentation dataset: owned partition has no body "
                        "reference";
                return false;
            }
            const std::optional<std::string> body =
                read_blob(node, partition->blob_ref());
            if (!body) {
                error = "segmentation dataset: owned partition '" +
                        partition->id() + "' has no local body";
                return false;
            }
            zetta::data::PartitionBodyReadResult decoded =
                zetta::data::read_partition_body(*body, partition->id());
            if (!decoded.ok()) {
                error = "segmentation dataset: " + decoded.error;
                return false;
            }
            if (decoded.rows.size() !=
                partition->row_range().end - partition->row_range().start) {
                error = "segmentation dataset: partition row count differs "
                        "from catalog";
                return false;
            }
            read_count += decoded.rows.size();
            for (const zetta::data::PartitionRow &partition_row :
                 decoded.rows) {
                if (!append_row(partition_row.doc, dataset.contract, params,
                                rows, error)) {
                    return false;
                }
            }
        }
        return true;
    }

    expected_count = table->store().size();
    // Unpartitioned table bodies are replicated. Read them on the root only.
    if (!is_root)
        return true;
    for (std::size_t index = 0; index < table->store().size(); ++index) {
        const zetta::storage::ParquetLazyPtr row = table->store().at(index);
        if (!row) {
            error = "segmentation dataset: could not read row " +
                    std::to_string(index) + ": " + row.error();
            return false;
        }
        ++read_count;
        if (!append_row(*row, dataset.contract, params, rows, error))
            return false;
    }
    return true;
}

void append_int64(std::vector<double> &output, std::int64_t value) {
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    output.push_back(static_cast<double>(bits >> 32));
    output.push_back(static_cast<double>(bits & 0xffffffffull));
}

std::int64_t read_int64(const std::vector<double> &input, std::size_t offset) {
    const auto high = static_cast<std::uint64_t>(input[offset]);
    const auto low = static_cast<std::uint64_t>(input[offset + 1]);
    return static_cast<std::int64_t>((high << 32) | low);
}

std::vector<double>
encode_assignments(const std::vector<SegmentAssignment> &assignments) {
    std::vector<double> encoded;
    encoded.push_back(static_cast<double>(assignments.size()));
    for (const SegmentAssignment &assignment : assignments) {
        append_int64(encoded, assignment.entity);
        encoded.push_back(static_cast<double>(assignment.segment_id));
        encoded.push_back(assignment.distance_to_center);
    }
    return encoded;
}

bool decode_assignments(const std::vector<double> &encoded,
                        std::vector<SegmentAssignment> &assignments) {
    if (encoded.empty())
        return true;
    if (!std::isfinite(encoded[0]) || encoded[0] < 0.0 ||
        encoded[0] > static_cast<double>(encoded.size()) ||
        std::trunc(encoded[0]) != encoded[0])
        return false;
    const std::size_t count = static_cast<std::size_t>(encoded[0]);
    if (encoded.size() != 1 + count * 4)
        return false;
    std::size_t offset = 1;
    for (std::size_t index = 0; index < count; ++index) {
        assignments.push_back({read_int64(encoded, offset),
                               static_cast<int>(encoded[offset + 2]),
                               encoded[offset + 3]});
        offset += 4;
    }
    return true;
}

std::string output_suffix(const std::string &model_id) {
    std::string suffix;
    for (const char character : model_id) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9'))
            suffix.push_back(character);
    }
    return suffix;
}

nlohmann::json model_feature_schema(const SegmentationResult &result) {
    nlohmann::json features = nlohmann::json::array();
    for (const SegmentationFeature &feature : result.model.features) {
        features.push_back(
            {{"source", feature.source},
             {"encoded_name", feature.encoded_name},
             {"kind", feature.kind == SegmentationFeatureKind::Numeric
                          ? "numeric"
                          : "categorical"},
             {"level", feature.level},
             {"location", feature.location},
             {"scale", feature.scale},
             {"robust", feature.robust},
             {"rare_bucket", feature.rare_bucket}});
    }
    nlohmann::json candidates = nlohmann::json::array();
    for (const auto &candidate : result.metrics.tried) {
        candidates.push_back(
            {{"k", candidate.k},
             {"inertia", candidate.inertia},
             {"silhouette", candidate.silhouette},
             {"davies_bouldin", candidate.davies_bouldin},
             {"minimum_share", candidate.minimum_share},
             {"satisfies_minimum_share", candidate.satisfies_minimum_share}});
    }
    return {{"features", std::move(features)},
            {"excluded_columns", result.model.excluded_columns},
            {"k_candidates", std::move(candidates)},
            {"chosen_k_reason", result.metrics.chosen_k_reason}};
}

nlohmann::json model_metrics(const SegmentationResult &result) {
    nlohmann::json metrics = {
        {"chosen_k", result.metrics.chosen_k},
        {"silhouette", result.metrics.silhouette},
        {"davies_bouldin", result.metrics.davies_bouldin},
        {"stability", result.metrics.stability},
    };
    for (const SegmentationKMetric &tried : result.metrics.tried) {
        metrics["inertia_k_" + std::to_string(tried.k)] = tried.inertia;
    }
    return metrics;
}

bool materialize_outputs(Node &node,
                         const zetta::data::ResolvedDataset &dataset,
                         const SegmentationRunOptions &options,
                         SegmentationRunResult &output) {
    std::shared_ptr<zetta::pipeline::Run> run =
        options.run_id.empty() ? node.create_run(options.project, options.graph)
                               : node.find_run(options.run_id);
    if (!run) {
        output.error = "segmentation: could not create the Run record";
        return false;
    }
    // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
    run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
    output.run_id = run->id();
    if (const auto job = node.find_job(output.job_id))
        run->set_trace_id(job->value().trace_id);
    run->set_status(zetta::pipeline::RunStatus::Running);

    ModelValue model_value;
    model_value.name = options.model_name;
    model_value.kind = "segmentation";
    model_value.project = options.project;
    model_value.run = run->id();
    model_value.feature_schema = model_feature_schema(output.segmentation);
    model_value.feature_schema["dataset"] = {
        {"table", dataset.database + "." + dataset.table},
        {"table_id", dataset.table_id},
        {"contract", zetta::data::to_json(dataset.contract)},
        {"notes", dataset.notes}};
    if (dataset.contract.entity_keys) {
        model_value.feature_schema["entity_key_mapping"] =
            dataset.contract.entity_keys->to_json();
        run->set_entity_key_mapping(dataset.contract.entity_keys->to_json());
    }
    model_value.metrics = model_metrics(output.segmentation);
    model_value.metrics["read_rows"] = output.read_rows;
    model_value.metrics["clustered_rows"] =
        output.segmentation.assignments.size();
    model_value.metrics["excluded_rows"] =
        output.read_rows - output.segmentation.assignments.size();
    const std::shared_ptr<Model> model =
        node.create_model(std::move(model_value));
    if (!model ||
        !model->set_weights(output.segmentation.model.to_json().dump())) {
        output.error = "segmentation: could not persist the segmentation Model";
        run->add_log({zetta::util::now_iso8601(), "error", output.error});
        run->set_status(zetta::pipeline::RunStatus::Failed);
        return false;
    }
    output.model_id = model->id();

    std::shared_ptr<zetta::data::Database> database =
        node.find_database(options.output_database);
    if (!database)
        database = node.create_database(options.output_database);
    if (!database) {
        output.error = "segmentation: could not create output database '" +
                       options.output_database + "'";
        run->add_log({zetta::util::now_iso8601(), "error", output.error});
        run->set_status(zetta::pipeline::RunStatus::Failed);
        return false;
    }
    const std::string suffix = output_suffix(model->id());
    const std::string assignment_name = "assignments_" + suffix;
    const std::string profile_name = "profiles_" + suffix;
    if (database->find_table(assignment_name) ||
        database->find_table(profile_name)) {
        output.error = "segmentation: output table name collision";
        run->set_status(zetta::pipeline::RunStatus::Failed);
        return false;
    }
    const std::shared_ptr<zetta::data::Table> assignments =
        database->create_table(assignment_name);
    const std::shared_ptr<zetta::data::Table> profiles =
        database->create_table(profile_name);
    if (!assignments || !profiles) {
        output.error =
            "segmentation: could not create assignment and profile tables";
        run->add_log({zetta::util::now_iso8601(), "error", output.error});
        run->set_status(zetta::pipeline::RunStatus::Failed);
        return false;
    }
    std::sort(
        output.segmentation.assignments.begin(),
        output.segmentation.assignments.end(),
        [](const SegmentAssignment &left, const SegmentAssignment &right) {
            return left.entity < right.entity;
        });
    for (const SegmentAssignment &assignment :
         output.segmentation.assignments) {
        assignments->add_row(
            {{dataset.contract.entity_column,
              zetta::data::entity_output(
                  dataset.contract, assignment.entity)},
             {"segment_id", assignment.segment_id},
             {"distance_to_center", assignment.distance_to_center}});
    }
    for (const SegmentProfile &profile : output.segmentation.profiles) {
        profiles->add_row(
            {{"segment_id", profile.segment_id},
             {"size", profile.size},
             {"share", profile.share},
             {"center", profile.center},
             {"distinguishing_features", profile.distinguishing_features},
             {"label", profile.label}});
    }
    output.assignment_table = options.output_database + "." + assignment_name;
    output.profile_table = options.output_database + "." + profile_name;

    run->set_outputs(
        {{"model", options.model_name, model->id()},
         {"dataset", "segment assignments", output.assignment_table},
         {"dataset", "segment profiles", output.profile_table},
         {"report", "run report", run->id()}});
    zetta::pipeline::RunLineage lineage;
    lineage.sources = {"dataset:" + dataset.database + "." + dataset.table};
    lineage.steps = {"global preprocessing", "k-means|| initialization",
                     "distributed Lloyd iterations", "segment profiling"};
    lineage.freshness = zetta::util::now_iso8601();
    lineage.confidence =
        std::clamp(output.segmentation.metrics.stability, 0.0, 1.0);
    run->set_lineage(std::move(lineage));
    run->add_log(
        {zetta::util::now_iso8601(), "info",
         "chosen k=" + std::to_string(output.segmentation.metrics.chosen_k) +
             ": " + output.segmentation.metrics.chosen_k_reason});
    if (options.run_id.empty()) {
        run->set_status(zetta::pipeline::RunStatus::Succeeded);
    }
    return true;
}

Outcome train_on_round(Node &node, const zetta::data::ResolvedDataset &dataset,
                       const SegmentationParams &params,
                       const SegmentationRunOptions &options, JobRound &round,
                       SegmentationRunResult &output, const std::string &tag) {
    output.is_root = round.is_root();
    std::vector<SegmentationRow> rows;
    std::uint64_t read_count = 0;
    std::uint64_t expected_count = 0;
    bool loaded = false;
    node.run_on_owner([&] {
        loaded =
            load_owned_rows(node, dataset, params, output.is_root, read_count,
                            expected_count, rows, output.error);
    });
    const auto readiness =
        round.allreduce(tag + ".readiness",
                        {loaded ? 0.0 : 1.0, static_cast<double>(read_count)},
                        zetta::cluster::CollectiveOp::Sum, options.timeout);
    if (!readiness || readiness->size() != 2 || (*readiness)[0] != 0.0 ||
        (*readiness)[1] != static_cast<double>(expected_count)) {
        return {.ok = false,
                .detail = output.error.empty()
                              ? "segmentation: incomplete or unreadable "
                                "owned partitions"
                              : output.error};
    }
    output.read_rows = static_cast<std::uint64_t>((*readiness)[1]);
    SegmentationCollectives collectives;
    collectives.allreduce = [&](const std::string &name,
                                const std::vector<double> &local,
                                SegmentationReduce operation) {
        return round.allreduce(name, local, collective_operation(operation),
                               options.timeout);
    };
    collectives.gather = [&](const std::string &name,
                             const std::vector<double> &local) {
        return round.gather(name, local, options.timeout);
    };
    collectives.broadcast = [&](const std::string &name,
                                const std::vector<double> &value) {
        return round.broadcast(name, value, options.timeout);
    };
    output.segmentation = train_segmentation(rows, params, collectives, tag);
    if (!output.segmentation.ok()) {
        return {.ok = false, .detail = output.segmentation.status.message};
    }
    auto &excluded = output.segmentation.model.excluded_columns;
    excluded.insert(excluded.end(), params.excluded_columns.begin(),
                    params.excluded_columns.end());
    excluded.push_back(params.entity_column);
    excluded.push_back(params.label_column);
    std::erase(excluded, std::string{});
    std::sort(excluded.begin(), excluded.end());
    excluded.erase(std::unique(excluded.begin(), excluded.end()),
                   excluded.end());

    return {.ok = true, .detail = {}};
}

Outcome persist_on_round(Node &node,
                         const zetta::data::ResolvedDataset &dataset,
                         const SegmentationRunOptions &options, JobRound &round,
                         SegmentationRunResult &output,
                         const std::string &tag) {
    const auto gathered = round.gather(
        tag + ".assignments",
        encode_assignments(output.segmentation.assignments), options.timeout);
    if (!gathered) {
        return {.ok = false,
                .detail = "segmentation: assignment gather failed"};
    }
    if (output.is_root) {
        output.segmentation.assignments.clear();
        for (const std::vector<double> &contribution : *gathered) {
            if (!decode_assignments(contribution,
                                    output.segmentation.assignments)) {
                return {.ok = false,
                        .detail =
                            "segmentation: malformed assignment contribution"};
            }
        }
    }
    // Final tables are deliberately materialized on one member. A registry
    // submission may originate off-root: send the gathered assignments through
    // the existing broadcast so that member can finish its already-visible Run.
    bool writer = output.is_root;
    if (!options.output_node.empty()) {
        node.run_on_owner([&] {
            writer =
                node.coordinated_launch() != nullptr &&
                node.coordinated_launch()->node_id() == options.output_node;
        });
        const auto assignments = round.broadcast(
            tag + ".writer_assignments",
            output.is_root ? encode_assignments(output.segmentation.assignments)
                           : std::vector<double>{},
            options.timeout);
        if (!assignments) {
            return {.ok = false,
                    .detail =
                        "segmentation: writer assignment broadcast failed"};
        }
        if (writer && !output.is_root) {
            output.segmentation.assignments.clear();
            if (!decode_assignments(*assignments,
                                    output.segmentation.assignments)) {
                return {.ok = false,
                        .detail = "segmentation: malformed writer assignments"};
            }
        }
    }
    bool written = true;
    if (writer) {
        node.run_on_owner([&] {
            written = materialize_outputs(node, dataset, options, output);
        });
    }
    const auto persisted =
        round.allreduce(tag + ".persisted", {writer && written ? 1.0 : 0.0},
                        zetta::cluster::CollectiveOp::Sum, options.timeout);
    if (!persisted || persisted->size() != 1 || (*persisted)[0] != 1.0) {
        return {.ok = false,
                .detail =
                    output.error.empty()
                        ? "segmentation: root output materialization failed"
                        : output.error};
    }
    return {.ok = true,
            .detail = "segmentation over " +
                      std::to_string(round.participants().size()) +
                      " nodes -> k=" +
                      std::to_string(output.segmentation.metrics.chosen_k) +
                      ", silhouette=" +
                      std::to_string(output.segmentation.metrics.silhouette)};
}

} // namespace

SegmentationRunResult
run_segmentation(Node &node, const zetta::data::ResolvedDataset &dataset,
                 SegmentationParams params,
                 const SegmentationRunOptions &options) {
    SegmentationRunResult output;
    if (!dataset.ok()) {
        output.error = "segmentation: unresolved dataset: " + dataset.error;
        output.status = compute::JobStatus::Failed;
        return output;
    }
    if (dataset.table_id.empty() || dataset.contract.entity_column.empty() ||
        dataset.contract.feature_columns.empty()) {
        output.error = "segmentation: the resolved dataset needs a table id, "
                       "entity column, and features";
        output.status = compute::JobStatus::Failed;
        return output;
    }
    compute::JobCoordinator *coordinator = nullptr;
    node.run_on_owner([&] {
        coordinator = node.job_coordinator();
    });
    if (coordinator == nullptr) {
        output.error = "segmentation: no compute plane attached to this node";
        output.status = compute::JobStatus::Failed;
        return output;
    }

    params.feature_columns = dataset.contract.feature_columns;
    params.entity_column = dataset.contract.entity_column;
    params.label_column = dataset.contract.label_column;
    params.excluded_columns.push_back(dataset.contract.time_column);

    std::shared_ptr<compute::ComputeJob> job;
    node.run_on_owner([&] {
        job = node.create_job(compute::JobKind::Train, options.project,
                              options.graph);
    });
    if (!job) {
        output.error = "segmentation: could not create a train ComputeJob";
        output.status = compute::JobStatus::Failed;
        return output;
    }
    output.job_id = job->id();
    const std::string tag =
        options.round_tag.empty()
            ? "ml.segmentation:" + dataset.database + "." + dataset.table
            : options.round_tag;
    output.status = coordinator->run(*job, [&](JobRound &round) -> Outcome {
        const auto trained =
            train_on_round(node, dataset, params, options, round, output, tag);
        if (!trained.ok)
            return trained;
        return persist_on_round(node, dataset, options, round, output, tag);
    });
    if (output.status != compute::JobStatus::Succeeded) {
        node.run_on_owner([&] {
            output.error = job->detail();
        });
        return output;
    }
    return output;
}

} // namespace zetta::ml
