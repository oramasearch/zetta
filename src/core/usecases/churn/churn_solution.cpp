#include "zetta/usecases/churn/churn_solution.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/usecases/churn/churn_step_executor.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

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

ChurnSolutionResult run_churn_solution(Node& node,
                                       compute::JobCoordinator& coordinator,
                                       compute::ComputeDevice& device,
                                       const ir::ExecutionGraph& graph,
                                       const ChurnDataset& dataset,
                                       const ChurnSolutionOptions& options) {
    const ChurnNumericShards shards = encode_churn_dataset(dataset);

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
    pipeline.project = options.project;
    pipeline.graph = options.graph;

    auto churn = std::make_shared<ChurnStepExecutor>(node, coordinator, device,
                                                     std::move(pipeline));
    zetta::pipeline::GraphExecutor executor{churn};

    ChurnSolutionResult result;
    result.execution = executor.execute(node, graph, options.project, options.graph);
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
    // caller can surface the reason or fall back to its own coordinator.
    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        ChurnSolutionResult result;
        result.execution.ok = false;
        result.execution.error =
            "no compute plane attached to this node (attach a cluster first) — "
            "cannot run over the live cluster";
        return result;
    }
    return run_churn_solution(node, *coordinator, device, graph, dataset, options);
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
            event.value = 1.0;
            dataset.events.push_back(event);
        }
    }
    return dataset;
}

}  // namespace zetta::usecases::churn