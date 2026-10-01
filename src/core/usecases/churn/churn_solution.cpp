#include "zetta/usecases/churn/churn_solution.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/usecases/churn/churn_step_executor.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::usecases::churn {
namespace {

using Json = nlohmann::json;
using ddf::ColumnSchema;
using ddf::DataFrameSchema;
using DataType = compute::DataType;

// Days since 1970-01-01 for a Gregorian (year, month, day) — Howard Hinnant's
// branch-free `days_from_civil`. A monotonic, calendar-correct date->integer map
// for any year, so a window `time <= as_of` includes exactly the events at or
// before t0 (the featurization cutoff) regardless of the absolute epoch.
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153u * (month + (month > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
    const unsigned day_of_era =
        year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    return era * 146097LL + static_cast<std::int64_t>(day_of_era) - 719468LL;
}

// Day-number of an ISO-8601 date/timestamp string (YYYY-MM-DD[...]). Tolerant of a
// trailing time component; returns 0 for a too-short/unparseable prefix (the value
// only needs to order events relative to the same-units as_of cutoff).
std::int64_t iso_date_to_day(const std::string& iso) {
    if (iso.size() < 10) return 0;
    const auto digits = [&](std::size_t at, std::size_t count) -> std::int64_t {
        std::int64_t value = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const char character = iso[at + index];
            if (character < '0' || character > '9') return -1;
            value = value * 10 + (character - '0');
        }
        return value;
    };
    const std::int64_t year = digits(0, 4);
    const std::int64_t month = digits(5, 2);
    const std::int64_t day = digits(8, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    return days_from_civil(year, static_cast<unsigned>(month),
                           static_cast<unsigned>(day));
}

// The categorical event type -> integer code the featurize spec references
// (failed_payments == 3 and support_ticket == 4 are the codes the windowed churn
// features key on). An unknown type maps to 0 (counted only by total_events).
std::int64_t event_type_code(const std::string& type) {
    if (type == "login") return 1;
    if (type == "feature_use") return 2;
    if (type == "payment_failed") return 3;
    if (type == "support_ticket") return 4;
    if (type == "csm_contact") return 5;
    if (type == "comms") return 6;
    return 0;
}

// Fill a JsonStore with synthetic row ids (the storage engine the loader reads).
zetta::storage::JsonStore make_store(const std::vector<Json>& rows) {
    zetta::storage::JsonStore store;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        store.add("row-" + std::to_string(index), rows[index]);
    }
    return store;
}

// This node's disjoint slice of the encoded dataset for a coordinated run
// (#601): the accounts whose global entity key lands on `rank` modulo `count`,
// and exactly the events of those accounts. Every participant encodes (or
// receives) the SAME full shards, so the slices are disjoint by construction
// and their union is the whole — the SPMD allreduce combines them to the global
// model without double-counting. count <= 1 keeps the whole dataset (the
// single-member / uncoordinated case).
ChurnNumericShards shard_for_rank(const ChurnNumericShards& full,
                                  std::size_t rank, std::size_t count) {
    if (count <= 1) return full;
    const auto owned = [&](const Json& row) {
        const std::int64_t entity = row.at("entity").get<std::int64_t>();
        return static_cast<std::size_t>(entity) % count == rank;
    };
    ChurnNumericShards shard;
    shard.as_of = full.as_of;
    for (const Json& account : full.accounts) {
        if (owned(account)) shard.accounts.push_back(account);
    }
    for (const Json& event : full.events) {
        if (owned(event)) shard.events.push_back(event);
    }
    return shard;
}

// The isolated-coordinator wiring (#637): the shared body every churn run goes
// through, over an explicitly-supplied coordinator and this node's (already
// encoded, already sharded) numeric slice. Deliberately NOT public — the one
// public entry is run_churn_solution_on_node, which resolves the node-owned
// spine, fans a multi-node launch out (#601), shards, and delegates here; the
// peer-side launch runner delegates here too.
ChurnSolutionResult run_churn_solution_with_coordinator(
    Node& node, compute::JobCoordinator& coordinator,
    compute::ComputeDevice& device, const ir::ExecutionGraph& graph,
    const ChurnNumericShards& shards, const ChurnSolutionOptions& options) {
    // The encoded shards must outlive execute() (the connect/clean steps load them
    // into DDF eagerly); local stores on this frame are exactly that lifetime.
    const zetta::storage::JsonStore events = make_store(shards.events);
    const zetta::storage::JsonStore accounts = make_store(shards.accounts);

    ChurnPipelineConfig pipeline;
    pipeline.events_store = &events;
    pipeline.events_schema = churn_events_schema();
    pipeline.accounts_store = &accounts;
    pipeline.accounts_schema = churn_accounts_schema();
    pipeline.featurize = churn_featurize_spec(shards.as_of);
    pipeline.params = options.params;
    pipeline.calibration = options.calibration;
    pipeline.entity_column = "entity";
    pipeline.top_k = options.top_k;
    pipeline.model_name = options.model_name;
    pipeline.result_database = options.result_database;
    pipeline.result_table_prefix = options.result_table_prefix;
    pipeline.result_entity_column = options.result_entity_column;
    pipeline.result_score_column = options.result_score_column;
    pipeline.project = options.project;
    pipeline.graph = options.graph;
    pipeline.training_progress = options.training_progress;

    auto churn = std::make_shared<ChurnStepExecutor>(node, coordinator, device,
                                                     std::move(pipeline));
    zetta::pipeline::GraphExecutor executor{churn};

    ChurnSolutionResult result;
    std::shared_ptr<zetta::pipeline::Run> run;
    if (!options.run_id.empty()) {
        node.run_on_owner([&] { run = node.find_run(options.run_id); });
    }
    result.execution = executor.execute(node, graph, options.project,
                                        options.graph, options.cancel,
                                        std::move(run));
    result.model_id = churn->model_id();
    result.ranked = churn->ranked();
    result.drivers = churn->driver_ranking();
    result.top_driver = churn->top_driver();
    result.playbook = churn->playbook();
    return result;
}

// ---- The coordinated-launch wire form (#601) --------------------------------

// Collects the first validation failure via the error-handler overload, so a
// validation failure never crosses the validator-library boundary as an
// exception (the churn-dataset / signal-envelope pattern).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

nlohmann::json_schema::json_validator& coordinated_run_validator() {
    static nlohmann::json_schema::json_validator validator(
        churn_coordinated_run_schema());
    return validator;
}

// The run options as carried in the launch payload. The GbdtParams keep their
// own validated boundary (to_json / from_json); the calibration config is the
// three-field value shape the payload schema pins down.
Json options_to_wire(const ChurnSolutionOptions& options) {
    return Json{
        {"params", options.params.to_json()},
        {"calibration",
         Json{{"enabled", options.calibration.enabled},
              {"method", ml::to_string(options.calibration.method)},
              {"holdout_fraction", options.calibration.holdout_fraction}}},
        {"top_k", options.top_k},
        {"model_name", options.model_name},
        {"project", options.project},
        {"graph_label", options.graph},
        {"result_database", options.result_database},
        {"result_table_prefix", options.result_table_prefix},
        {"result_entity_column", options.result_entity_column},
        {"result_score_column", options.result_score_column}};
}

// Parse the (schema-validated) options object back. Returns nullopt + a reason
// only for the parts with their own deeper boundary (the GbdtParams).
std::optional<ChurnSolutionOptions> options_from_wire(const Json& wire,
                                                      std::string& error) {
    ChurnSolutionOptions options;
    const std::optional<ml::GbdtParams> params =
        ml::GbdtParams::from_json(wire.at("params"));
    if (!params) {
        error = "options.params is not a valid GbdtParams object";
        return std::nullopt;
    }
    options.params = *params;
    const Json& calibration = wire.at("calibration");
    options.calibration.enabled = calibration.at("enabled").get<bool>();
    options.calibration.method =
        ml::calibration_method_from_string(
            calibration.at("method").get<std::string>())
            .value_or(ml::CalibrationMethod::Platt);
    options.calibration.holdout_fraction =
        calibration.at("holdout_fraction").get<double>();
    options.top_k = wire.at("top_k").get<std::size_t>();
    options.model_name = wire.at("model_name").get<std::string>();
    if (wire.contains("project")) options.project = wire["project"].get<std::string>();
    if (wire.contains("graph_label")) options.graph = wire["graph_label"].get<std::string>();
    options.result_database = wire.value("result_database", options.result_database);
    options.result_table_prefix = wire.value("result_table_prefix", options.result_table_prefix);
    options.result_entity_column = wire.value("result_entity_column", options.result_entity_column);
    options.result_score_column = wire.value("result_score_column", options.result_score_column);
    options.coordinate = false;  // a co-launched run never re-fans-out
    return options;
}

// The peer-side body of a `run.launch` churn task (#601), executed on the
// node-owning thread by the listener's drain. Every decline is a reason the
// drain span records — never a silent no-op (the initiator's collective rounds
// surface the missing peer with the bounded timeout either way). `self` is this
// node's id in the ROSTER's domain — the cluster node id, from the listener.
compute::CoordinatedLaunchListener::LaunchOutcome run_coordinated_churn_launch(
    Node& node, const std::string& self,
    const compute::CoordinatedLaunchRequest& request) {
    using LaunchOutcome = compute::CoordinatedLaunchListener::LaunchOutcome;

    // 1) The payload boundary: schema-validate before reading a field.
    collecting_error_handler handler;
    coordinated_run_validator().validate(request.payload, handler);
    if (handler.failed) {
        return LaunchOutcome{.ok = false,
                             .detail = "churn launch payload: " + handler.reason};
    }

    // 2) The graph rides the IR's own validated boundary.
    std::string graph_error;
    const std::optional<ir::ExecutionGraph> graph =
        ir::from_json(request.payload.at("graph"), &graph_error);
    if (!graph) {
        return LaunchOutcome{.ok = false,
                             .detail = "churn launch graph: " + graph_error};
    }
    if (!is_churn_graph(*graph)) {
        return LaunchOutcome{
            .ok = false,
            .detail = "churn launch graph is not churn-shaped "
                      "(connect + clean + model backbone required)"};
    }

    // 3) RULE-1 holds on every member: re-run the correctness gate here — a
    // peer never trusts the initiator's gating for the work IT performs.
    const zetta::pipeline::ValidationReport gate =
        zetta::pipeline::default_gate().run(*graph);
    if (!gate.passed() || gate.block_deploy) {
        std::string reason = "gate blocked the co-run";
        for (const zetta::pipeline::CheckResult& check : gate.checks) {
            if (check.status == zetta::pipeline::CheckStatus::Fail) {
                reason = check.name + ": " + check.reason;
            }
        }
        return LaunchOutcome{.ok = false, .detail = reason};
    }

    // 4) This node's shard rank comes from the agreed roster.
    const std::optional<std::size_t> rank =
        compute::participant_rank(request, self);
    if (!rank) {
        return LaunchOutcome{
            .ok = false,
            .detail = "this node is not in the launch roster (stale membership)"};
    }

    std::string options_error;
    std::optional<ChurnSolutionOptions> options =
        options_from_wire(request.payload.at("options"), options_error);
    if (!options) return LaunchOutcome{.ok = false, .detail = options_error};

    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        return LaunchOutcome{.ok = false,
                             .detail = "no compute plane attached to this node"};
    }

    // 5) Rebuild the full encoded shards from the payload, slice this node's
    // disjoint entity shard, and co-run — the same body the initiator runs.
    ChurnNumericShards full;
    full.as_of = request.payload.at("as_of").get<double>();
    for (const Json& event : request.payload.at("events")) full.events.push_back(event);
    for (const Json& account : request.payload.at("accounts")) full.accounts.push_back(account);
    const ChurnNumericShards mine =
        shard_for_rank(full, *rank, request.participants.size());

    compute::CpuDevice device;
    const ChurnSolutionResult result = run_churn_solution_with_coordinator(
        node, *coordinator, device, *graph, mine, *options);
    if (!result.ok()) {
        return LaunchOutcome{.ok = false, .detail = result.execution.error};
    }
    return LaunchOutcome{
        .ok = true,
        .detail = "co-ran churn graph over shard " + std::to_string(*rank) +
                  "/" + std::to_string(request.participants.size()) + " (" +
                  std::to_string(mine.accounts.size()) + " accounts)"};
}

}  // namespace

ml::GbdtParams default_churn_gbdt_params() {
    ml::GbdtParams params;
    params.num_trees = 30;
    params.max_depth = 3;
    params.max_bins = 16;
    params.learning_rate = 0.3;
    params.min_child_hessian = 1e-3;
    return params;
}

DataFrameSchema churn_events_schema() {
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "entity", .dtype = DataType::Int64},
                      ColumnSchema{.name = "ts", .dtype = DataType::Int64},
                      ColumnSchema{.name = "value", .dtype = DataType::Float64},
                      ColumnSchema{.name = "type", .dtype = DataType::Int64}};
    return schema;
}

DataFrameSchema churn_accounts_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "entity", .dtype = DataType::Int64},
        ColumnSchema{.name = "mrr", .dtype = DataType::Float64},
        ColumnSchema{.name = "seats", .dtype = DataType::Int64},
        ColumnSchema{.name = "tenure_days", .dtype = DataType::Int64},
        ColumnSchema{.name = "churned", .dtype = DataType::Int64}};
    return schema;
}

ddf::FeaturizeSpec churn_featurize_spec(double as_of) {
    ddf::FeaturizeSpec spec;
    spec.entity_column = "entity";
    spec.time_column = "ts";
    spec.as_of = as_of;
    spec.event_features = {
        ddf::WindowedFeature{.name = "failed_payments",
                             .agg = ddf::AggregateOp::Count,
                             .type_column = "type",
                             .type_value = 3.0},
        ddf::WindowedFeature{.name = "support_severity",
                             .agg = ddf::AggregateOp::Sum,
                             .value_column = "value",
                             .type_column = "type",
                             .type_value = 4.0},
        ddf::WindowedFeature{.name = "total_events",
                             .agg = ddf::AggregateOp::Count},
    };
    spec.account_features = {"mrr", "seats", "tenure_days"};
    spec.label_column = "churned";
    return spec;
}

ChurnNumericShards encode_churn_dataset(const ChurnDataset& dataset) {
    ChurnNumericShards shards;

    // Dense integer entity keys by file order (account[i] -> i); a lookup table maps
    // an event's account_id back to its key.
    std::map<std::string, std::int64_t> entity_index;
    shards.accounts.reserve(dataset.accounts.size());
    for (const ChurnAccount& account : dataset.accounts) {
        const std::int64_t key = static_cast<std::int64_t>(entity_index.size());
        entity_index.emplace(account.account_id, key);
        shards.accounts.push_back(Json{{"entity", key},
                                       {"mrr", account.mrr},
                                       {"seats", account.seats},
                                       {"tenure_days", account.tenure_days},
                                       {"churned", account.churned ? 1 : 0}});
    }

    shards.events.reserve(dataset.events.size());
    for (const ChurnEvent& event : dataset.events) {
        const auto found = entity_index.find(event.account_id);
        if (found == entity_index.end()) continue;  // event for an unknown account
        shards.events.push_back(
            Json{{"entity", found->second},
                 {"ts", iso_date_to_day(event.event_ts)},
                 {"value", event.value},
                 {"type", event_type_code(event.event_type)}});
    }

    shards.as_of = static_cast<double>(iso_date_to_day(dataset.as_of_date));
    return shards;
}

DataFrameSchema table_accounts_schema(
    const zetta::data::DatasetContract& contract,
    bool continuous_label) {
    DataFrameSchema schema;
    // `field` is the JSON key read from the row, `name` the column in the frame.
    // The entity is renamed to the canonical `entity` on the way in: everything
    // downstream of featurize — the feature frame, the score job, the ranked list —
    // names it that, and threading a per-customer spelling through all of them
    // would be a second name for one concept.
    schema.columns.push_back(ColumnSchema{.name = std::string(kTableEntityColumn),
                                          .field = contract.entity_column,
                                          .dtype = DataType::Int64,
                                          .entity_keys = contract.entity_keys});
    for (const std::string& feature : contract.feature_columns) {
        schema.columns.push_back(
            ColumnSchema{.name = feature, .field = feature, .dtype = DataType::Float64});
    }
    if (!contract.label_column.empty()) {
        schema.columns.push_back(ColumnSchema{.name = contract.label_column,
                                              .field = contract.label_column,
                                              .dtype = continuous_label
                                                           ? DataType::Float64
                                                           : DataType::Int64});
    }
    if (!contract.time_column.empty()) {
        schema.columns.push_back(ColumnSchema{.name = contract.time_column,
                                              .field = contract.time_column,
                                              .dtype = DataType::Float64,
                                              .timestamp = true});
    }
    return schema;
}

ddf::FeaturizeSpec table_featurize_spec(const zetta::data::DatasetContract& contract) {
    ddf::FeaturizeSpec spec;
    spec.entity_column = std::string(kTableEntityColumn);
    spec.time_column = contract.time_column;
    spec.as_of = contract.as_of;
    // Static carry-through: no windowed event features. A connector query returns
    // one already-featurized row per entity — the windowing happened in the
    // warehouse SQL, and inventing event aggregates over rows that are not events
    // would be arithmetic on a shape that does not exist.
    spec.account_features = contract.feature_columns;
    spec.label_column = contract.label_column;
    return spec;
}

ChurnSolutionResult run_churn_solution_on_table(
    Node& node, compute::ComputeDevice& device, const ir::ExecutionGraph& graph,
    const ChurnTableInput& input, const ChurnSolutionOptions& options) {
    const auto fail = [](std::string reason) {
        ChurnSolutionResult result;
        result.execution.ok = false;
        result.execution.error = std::move(reason);
        return result;
    };
    if (input.empty()) return fail("no dataset table given");
    if (input.contract.entity_column.empty() || input.contract.label_column.empty()) {
        return fail(
            "the dataset contract needs an entity column and a label column — resolve "
            "it with zetta::data::resolve_dataset");
    }
    if (input.contract.feature_columns.empty()) {
        return fail("the dataset contract names no feature columns");
    }

    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        return fail(
            "no compute plane attached to this node (attach_cluster first) — the churn "
            "pipeline runs on the node's live JobCoordinator");
    }

    // The SPMD rounds need every participant co-running. The in-memory path fans a
    // launch out over the signal plane; the table path's fan-out is not wired yet,
    // so say so rather than train one node's shard and report it as the whole.
    const std::vector<std::string> roster = coordinator->collectives().participants();
    if (options.coordinate && roster.size() > 1) {
        return fail(
            "running a connector-landed table across " + std::to_string(roster.size()) +
            " nodes is not wired yet — each node would train on its own partitions "
            "with nothing combining them. Run it on a single-member cluster, or use "
            "the in-memory dataset path.");
    }

    const zetta::storage::JsonStore forward_labels =
        make_store(input.forward_label_rows);
    ChurnPipelineConfig pipeline;
    pipeline.accounts_table = input.table_id;
    pipeline.accounts_dataset = input.dataset_name;
    pipeline.dataset_notes = input.notes;
    pipeline.entity_keys = input.contract.entity_keys;
    pipeline.accounts_schema = table_accounts_schema(
        input.contract, options.params.objective == "squared_error");
    pipeline.events_schema = churn_events_schema();  // empty frame; static mode
    pipeline.featurize = table_featurize_spec(input.contract);
    if (!input.forward_label_rows.empty()) {
        pipeline.training_accounts_store = &forward_labels;
        pipeline.training_accounts_schema = table_accounts_schema(
            input.forward_label_contract,
            options.params.objective == "squared_error");
        pipeline.training_featurize =
            table_featurize_spec(input.forward_label_contract);
        pipeline.training_dataset = input.forward_label_source;
        pipeline.score_active_only = input.score_active_only;
    }
    pipeline.params = options.params;
    pipeline.calibration = options.calibration;
    pipeline.entity_column = std::string(kTableEntityColumn);
    pipeline.top_k = options.top_k;
    pipeline.model_name = options.model_name;
    pipeline.result_database = options.result_database;
    pipeline.result_table_prefix = options.result_table_prefix;
    pipeline.result_entity_column = options.result_entity_column;
    pipeline.result_score_column = options.result_score_column;
    pipeline.project = options.project;
    pipeline.graph = options.graph;
    pipeline.training_progress = options.training_progress;

    auto churn = std::make_shared<ChurnStepExecutor>(node, *coordinator, device,
                                                     std::move(pipeline));
    zetta::pipeline::GraphExecutor executor{churn};

    ChurnSolutionResult result;
    std::shared_ptr<zetta::pipeline::Run> run;
    if (!options.run_id.empty()) {
        node.run_on_owner([&] { run = node.find_run(options.run_id); });
    }
    result.execution = executor.execute(node, graph, options.project, options.graph,
                                        options.cancel, std::move(run));
    result.model_id = churn->model_id();
    result.ranked = churn->ranked();
    result.drivers = churn->driver_ranking();
    result.top_driver = churn->top_driver();
    result.playbook = churn->playbook();
    return result;
}

ChurnSolutionResult run_churn_solution_on_node(
    Node& node, compute::ComputeDevice& device,
    const ir::ExecutionGraph& graph, const ChurnDataset& dataset,
    const ChurnSolutionOptions& options) {
    // Resolve the node's live compute spine (#579). A node that has not been
    // attach_cluster'd has none — fail clearly rather than silently no-op, so the
    // caller can surface the reason.
    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        ChurnSolutionResult result;
        result.execution.ok = false;
        result.execution.error =
            "no compute plane attached to this node (attach a cluster first) — "
            "cannot run over the live cluster";
        return result;
    }

    const ChurnNumericShards full = encode_churn_dataset(dataset);

    // The coordinated cross-process launch (#601): with live peers, the SPMD
    // collective rounds need every member co-running — fan the launch out over
    // the signal plane, then run this node's own disjoint shard. The roster is
    // the collective participant set (this node + connected peers, node-id
    // ascending — identical on every node), snapshotted once here so every
    // participant agrees on the shard arithmetic.
    const std::vector<std::string> roster = coordinator->collectives().participants();
    if (options.coordinate && roster.size() > 1) {
        compute::CoordinatedLaunchListener* listener = node.coordinated_launch();
        if (listener == nullptr) {
            ChurnSolutionResult result;
            result.execution.ok = false;
            result.execution.error =
                "the cluster has peers but this node has no run.launch listener "
                "(attach the cluster through Node::attach_cluster) — a lone "
                "submitter would fail the SPMD collective rounds";
            return result;
        }
        compute::CoordinatedLaunchRequest request;
        request.id = zetta::util::new_uuid();
        request.kind = std::string(kChurnLaunchKind);
        // Ids in the roster's domain: the CLUSTER node id (the Node's own id
        // may differ from its cluster identity, e.g. in the loopback harness).
        request.initiator = listener->node_id();
        request.participants = roster;
        request.payload = Json{{"graph", ir::to_json(graph)},
                               {"as_of", full.as_of},
                               {"events", Json(full.events)},
                               {"accounts", Json(full.accounts)},
                               {"options", options_to_wire(options)}};
        listener->fan_out(request);
        const std::size_t rank =
            compute::participant_rank(request, listener->node_id()).value_or(0);
        return run_churn_solution_with_coordinator(
            node, *coordinator, device, graph,
            shard_for_rank(full, rank, roster.size()), options);
    }

    return run_churn_solution_with_coordinator(node, *coordinator, device, graph,
                                               full, options);
}

const nlohmann::json& churn_coordinated_run_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ChurnCoordinatedRun",
      "description": "The churn-solution run.launch payload (#601): the graph, the full encoded numeric shards (each participant slices its own disjoint entity shard), the as_of cutoff, and the run options.",
      "type": "object",
      "required": ["graph", "as_of", "events", "accounts", "options"],
      "additionalProperties": false,
      "properties": {
        "graph":  { "type": "object" },
        "as_of":  { "type": "number" },
        "events": { "type": "array", "items": {
          "type": "object",
          "required": ["entity", "ts", "value", "type"],
          "additionalProperties": false,
          "properties": {
            "entity": { "type": "integer", "minimum": 0 },
            "ts":     { "type": "integer" },
            "value":  { "type": "number" },
            "type":   { "type": "integer" }
          } } },
        "accounts": { "type": "array", "items": {
          "type": "object",
          "required": ["entity", "mrr", "seats", "tenure_days", "churned"],
          "additionalProperties": false,
          "properties": {
            "entity":      { "type": "integer", "minimum": 0 },
            "mrr":         { "type": "number" },
            "seats":       { "type": "integer" },
            "tenure_days": { "type": "integer" },
            "churned":     { "type": "integer", "minimum": 0, "maximum": 1 }
          } } },
        "options": {
          "type": "object",
          "required": ["params", "calibration", "top_k", "model_name"],
          "additionalProperties": false,
          "properties": {
            "params": { "type": "object" },
            "calibration": {
              "type": "object",
              "required": ["enabled", "method", "holdout_fraction"],
              "additionalProperties": false,
              "properties": {
                "enabled":          { "type": "boolean" },
                "method":           { "type": "string",
                                      "enum": ["platt", "isotonic"] },
                "holdout_fraction": { "type": "number",
                                      "minimum": 0, "maximum": 1 }
              } },
            "top_k":       { "type": "integer", "minimum": 0 },
            "model_name":  { "type": "string", "minLength": 1 },
            "project":     { "type": "string" },
            "graph_label": { "type": "string" },
            "result_database": { "type": "string", "minLength": 1 },
            "result_table_prefix": { "type": "string", "minLength": 1 },
            "result_entity_column": { "type": "string", "minLength": 1 },
            "result_score_column": { "type": "string", "minLength": 1 }
          } }
      }
    })JSONSCHEMA");
    return schema;
}

void install_churn_launch_runner(compute::CoordinatedLaunchListener& listener,
                                 Node& node) {
    // The runner is stored in (and so cannot outlive) the listener it captures.
    listener.set_runner(
        std::string(kChurnLaunchKind),
        [&node, &listener](const compute::CoordinatedLaunchRequest& request) {
            return run_coordinated_churn_launch(node, listener.node_id(),
                                                request);
        });
}

std::shared_ptr<AsyncChurnRun> AsyncChurnRun::start(Node& node,
                                                    ir::ExecutionGraph graph,
                                                    ChurnDataset dataset,
                                                    ChurnSolutionOptions options) {
    // No make_shared: the constructor is private (start() is the one way in,
    // so a handle always has its worker running).
    return std::shared_ptr<AsyncChurnRun>(new AsyncChurnRun(
        node, std::move(graph), std::move(dataset), {}, std::move(options)));
}

std::shared_ptr<AsyncChurnRun> AsyncChurnRun::start(Node& node,
                                                    ir::ExecutionGraph graph,
                                                    ChurnTableInput table,
                                                    ChurnSolutionOptions options) {
    return std::shared_ptr<AsyncChurnRun>(new AsyncChurnRun(
        node, std::move(graph), {}, std::move(table), std::move(options)));
}

AsyncChurnRun::AsyncChurnRun(Node& node, ir::ExecutionGraph graph,
                             ChurnDataset dataset, ChurnTableInput table,
                             ChurnSolutionOptions options)
    : node_(node),
      graph_(std::move(graph)),
      dataset_(std::move(dataset)),
      table_(std::move(table)),
      options_(std::move(options)),
      project_(options_.project),
      device_(std::make_unique<compute::CpuDevice>()),
      started_(std::chrono::steady_clock::now()) {
    // The worker's cancel flag is this handle's — wire it before launch.
    options_.cancel = &cancel_;
    worker_ = std::thread([this] {
        // Everything the worker reads it owns (the by-value snapshots above);
        // every node-tree write on this path gates through Node::run_on_owner.
        result_ = table_.empty() ? run_churn_solution_on_node(node_, *device_, graph_,
                                                              dataset_, options_)
                                 : run_churn_solution_on_table(node_, *device_, graph_,
                                                               table_, options_);
        done_.store(true, std::memory_order_release);  // last act
    });
}

AsyncChurnRun::~AsyncChurnRun() {
    request_cancel();
    // Pump the node's owner gate while the worker winds down: it may be
    // blocked handing a node write to the owner loop (this thread), and
    // joining without draining would deadlock. Cancel lands at the next
    // between-steps check, so this waits at most one step.
    while (!done_.load(std::memory_order_acquire)) {
        node_.drain_owner_work();
        std::this_thread::yield();
    }
    if (worker_.joinable()) worker_.join();
}

bool is_churn_graph(const ir::ExecutionGraph& graph) {
    // The featurize -> train backbone the ChurnStepExecutor pipeline needs: a source
    // to load (connect), a featurize (clean), and a train (model). A graph missing any
    // of these can't drive the real churn run and stays on DefaultStepExecutor.
    bool connect = false, clean = false, model = false;
    for (const std::shared_ptr<ir::Node>& step : graph.nodes()) {
        switch (step->kind) {
            case ir::StepKind::Connect: connect = true; break;
            case ir::StepKind::Clean:   clean = true; break;
            case ir::StepKind::Model:   model = true; break;
            default: break;
        }
    }
    return connect && clean && model;
}

ChurnDataset demo_churn_dataset() {
    ChurnDataset dataset;
    dataset.name = "demo_churn";
    dataset.description = "Built-in synthetic churn demo (no files).";
    dataset.entity_field = "account_id";
    dataset.time_field = "event_ts";
    dataset.label_field = "churned";
    dataset.as_of_date = "2026-06-01";
    dataset.churn_horizon_days = 30;
    dataset.label_definition = "account churned within the horizon after t0";

    const auto two_digit = [](int value) {
        std::string text = std::to_string(value);
        return text.size() < 2 ? "0" + text : text;
    };

    constexpr int kAccounts = 24;
    int event_serial = 0;
    const auto next_event_id = [&]() {
        return "evt_" + std::to_string(event_serial++);
    };

    for (int index = 0; index < kAccounts; ++index) {
        // Alternate the label so it is NOT correlated with the index-derived static
        // features (mrr/seats/tenure) — only the payment-failure signal separates
        // the classes, so failed_payments is the dominant driver.
        const bool churned = (index % 2 == 1);
        const std::string account_id = "acct_" + two_digit(index);

        ChurnAccount account;
        account.account_id = account_id;
        account.name = "Demo Account " + two_digit(index);
        account.entity = account_id;
        account.plan_tier =
            (index % 3 == 0) ? "starter" : (index % 3 == 1 ? "business" : "enterprise");
        account.industry = "demo";
        account.region = "demo";
        account.mrr = 500.0 + 37.0 * index;
        account.seats = 5 + index;
        account.tenure_days = 120 + 11 * index;
        account.renewal_date = "2026-09-01";
        account.churned = churned;
        if (churned) account.churn_date = "2026-06-20";
        dataset.accounts.push_back(account);

        // Baseline engagement: a varying number of feature-use events (5..8), so
        // total_events overlaps across classes and does not cleanly separate them.
        const int baseline = 5 + (index % 4);
        for (int occurrence = 0; occurrence < baseline; ++occurrence) {
            const int month = 1 + (occurrence % 5);  // Jan..May, all before t0
            ChurnEvent event;
            event.event_id = next_event_id();
            event.account_id = account_id;
            event.event_ts = "2026-" + two_digit(month) + "-15T12:00:00Z";
            event.event_type = (occurrence == 0) ? "login" : "feature_use";
            // The schema requires a non-empty channel (the demo dataset must be
            // schema-valid: write_churn_dataset round-trips it, #623).
            event.channel = "app";
            event.value = 1.0;
            dataset.events.push_back(event);
        }

        // The churn signal: churned accounts accumulate 2..4 payment failures before
        // t0; healthy accounts have none.
        const int failures = churned ? (2 + (index % 3)) : 0;
        for (int occurrence = 0; occurrence < failures; ++occurrence) {
            const int month = 2 + occurrence;  // Feb..May, all before t0
            ChurnEvent event;
            event.event_id = next_event_id();
            event.account_id = account_id;
            event.event_ts = "2026-" + two_digit(month) + "-10T09:00:00Z";
            event.event_type = "payment_failed";
            event.channel = "billing";
            event.value = 1.0;
            dataset.events.push_back(event);
        }
    }
    return dataset;
}

}  // namespace zetta::usecases::churn
