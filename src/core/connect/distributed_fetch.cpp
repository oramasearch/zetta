#include "zetta/connect/distributed_fetch.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

#include "snowflake_protocol.hpp"
#include "zetta/runtime.hpp"

namespace zetta::connect {
namespace {

// How long to wait for an asynchronously-executing statement before giving up.
// The SQL API answers 202 while a statement is still running; we poll its handle.
constexpr int kMaxAsyncPolls = 40;
constexpr int kAsyncPollMillis = 500;

// The ceiling on one node's in-flight partition reads, whatever a caller asks
// for. A fan-out is a fan-out, not a socket storm.
constexpr std::size_t kMaxPartitionsInFlight = 32;

}  // namespace

std::vector<PartitionAssignment> plan_partition_assignments(
    const std::vector<std::string>& nodes,
    const std::vector<std::uint64_t>& partition_rows,
    const std::vector<double>& node_weights) {
    std::vector<PartitionAssignment> plan;
    if (nodes.empty()) return plan;

    plan.reserve(nodes.size());
    for (const std::string& node : nodes) {
        plan.push_back(PartitionAssignment{node, {}, 0});
    }

    // The share each node is meant to end up with. Equal by default; the
    // partition plan's per-node primary count when the caller passed one, which
    // is what lands the fetch directly in the target shape. A weight set that is
    // the wrong length, or all zeros, is ignored rather than obeyed — a fetch
    // nobody is assigned anything in is worse than an even one.
    std::vector<double> weights(nodes.size(), 1.0);
    if (node_weights.size() == nodes.size()) {
        double total = 0.0;
        for (const double weight : node_weights) total += std::max(weight, 0.0);
        if (total > 0.0) {
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                weights[index] = std::max(node_weights[index], 0.0);
            }
        }
    }

    // Longest-processing-time-first: hand the biggest partition to whichever
    // node is currently carrying the fewest rows *relative to its share*.
    // Balancing by ROW COUNT rather than by index is the whole point — result
    // partitions are uneven, so a round-robin over indices leaves one node
    // holding the tail and the fetch takes as long as that node's link.
    std::vector<std::size_t> order(partition_rows.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(),
                     [&partition_rows](std::size_t left, std::size_t right) {
                         return partition_rows[left] > partition_rows[right];
                     });

    // A zero-weighted node is not a candidate at all (the plan gave it no
    // partitions to own, so pulling rows onto it would land them nowhere).
    const auto load_of = [&](std::size_t index) {
        return weights[index] <= 0.0
                   ? std::numeric_limits<double>::max()
                   : static_cast<double>(plan[index].rows) / weights[index];
    };
    for (const std::size_t partition : order) {
        // Ties break on the first (lowest) node, so the plan is stable and
        // identical on every node that computes it from the same inputs.
        std::size_t lightest = 0;
        for (std::size_t candidate = 1; candidate < plan.size(); ++candidate) {
            if (load_of(candidate) < load_of(lightest)) lightest = candidate;
        }
        if (load_of(lightest) == std::numeric_limits<double>::max()) {
            lightest = 0;  // every weight was zero: fall back to node order
        }
        plan[lightest].partitions.push_back(partition);
        plan[lightest].rows += partition_rows[partition];
    }

    // Ascending within a slice: a node fetches its partitions in result order,
    // which keeps the landed rows in a predictable sequence.
    for (PartitionAssignment& assignment : plan) {
        std::sort(assignment.partitions.begin(), assignment.partitions.end());
    }
    return plan;
}

std::vector<PartitionAssignment> plan_fetch_assignments(
    const std::vector<std::string>& roster,
    const std::vector<std::uint64_t>& partition_rows,
    const std::vector<double>& node_weights) {
    // Partition 0 belongs to the initiator by construction — it came inline with
    // the submission. Tile only what is still to be fetched.
    const std::vector<std::uint64_t> fetchable =
        partition_rows.empty()
            ? std::vector<std::uint64_t>{}
            : std::vector<std::uint64_t>(partition_rows.begin() + 1,
                                         partition_rows.end());
    std::vector<PartitionAssignment> plan =
        plan_partition_assignments(roster, fetchable, node_weights);
    // Back into the original partition space, so a slice names the indices the
    // source is actually read by.
    for (PartitionAssignment& assignment : plan) {
        for (std::size_t& partition : assignment.partitions) ++partition;
    }
    return plan;
}

runtime::task<StatementSubmission> submit_connector_statement_async(
    ConnectorConfig config, std::string sql, std::size_t max_rows,
    HttpTransport transport) {
    StatementSubmission submission;
    if (!transport) {
        submission.error =
            "no outbound HTTP transport is installed in this build — a "
            "connector query cannot reach an external source";
        co_return submission;
    }

    HttpRequest request;
    request.method = "POST";
    request.url = snowflake::statements_url(config);
    request.headers = snowflake::headers(config);
    request.secret_headers = {"Authorization"};
    nlohmann::json payload{{"statement", sql}, {"timeout", 60}};
    if (!config.database.empty()) payload["database"] = config.database;
    if (!config.schema.empty()) payload["schema"] = config.schema;
    if (!config.warehouse.empty()) payload["warehouse"] = config.warehouse;
    if (!config.role.empty()) payload["role"] = config.role;
    request.body = payload.dump();

    HttpResponse response = transport(request);
    if (!response.error.empty()) {
        submission.error = "snowflake: " + response.error;
        co_return submission;
    }

    // 202 => still executing: poll the statement handle until it completes.
    int polls = 0;
    while (response.status == 202) {
        const auto body =
            nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
        const std::string handle = snowflake::statement_handle(body);
        if (handle.empty()) {
            submission.error =
                "snowflake: async response carried no statementHandle";
            co_return submission;
        }
        if (++polls > kMaxAsyncPolls) {
            submission.error =
                "snowflake: statement still running after " +
                std::to_string(kMaxAsyncPolls * kAsyncPollMillis / 1000) +
                "s (narrow the query, or pre-materialize it in the warehouse)";
            co_return submission;
        }
        // A TIMER, not a sleep (#832): the coroutine suspends on the shared
        // timer service and its thread goes back to other work. A blocking
        // sleep_for here pinned a thread for the whole duration of a slow
        // warehouse statement doing nothing at all.
        [[maybe_unused]] const auto waited =
            co_await runtime::delay(std::chrono::milliseconds(kAsyncPollMillis));
        response = transport(snowflake::partition_request(config, handle, -1));
        if (!response.error.empty()) {
            submission.error = "snowflake: " + response.error;
            co_return submission;
        }
    }

    if (!response.ok()) {
        submission.error = snowflake::status_error(response);
        co_return submission;
    }

    const auto body =
        nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    if (!body.is_object()) {
        submission.error = "snowflake: response body is not a JSON object";
        co_return submission;
    }
    if (!snowflake::read_row_types(body, submission.plan.columns,
                                   submission.plan.column_types,
                                   submission.error)) {
        co_return submission;
    }
    submission.plan.handle = snowflake::statement_handle(body);
    submission.plan.partition_rows = snowflake::partition_row_counts(body);

    submission.inline_rows.columns = submission.plan.columns;
    snowflake::append_data(body, submission.plan.columns,
                           submission.plan.column_types, max_rows,
                           submission.inline_rows.rows);
    co_return submission;
}

StatementSubmission submit_connector_statement(const ConnectorConfig& config,
                                               const std::string& sql,
                                               std::size_t max_rows,
                                               const HttpTransport& transport) {
    // The one explicit bridge for callers that are not coroutines. Everything
    // that can await should call submit_connector_statement_async directly.
    return runtime::block_on(
        submit_connector_statement_async(config, sql, max_rows, transport));
}

PartitionFetchResult fetch_result_partitions_serially(
    const ConnectorConfig& config, const StatementPlan& plan,
    const std::vector<std::size_t>& partitions, std::size_t max_rows,
    const HttpTransport& transport) {
    PartitionFetchResult result;
    if (partitions.empty()) return result;
    if (!transport) {
        result.error =
            "no outbound HTTP transport is installed in this build — a "
            "connector query cannot reach an external source";
        return result;
    }
    if (plan.handle.empty()) {
        result.error = "snowflake: the statement carried no handle to fetch by";
        return result;
    }

    for (const std::size_t partition : partitions) {
        if (result.rows.size() >= max_rows) break;
        const HttpResponse page = transport(snowflake::partition_request(
            config, plan.handle, static_cast<long long>(partition)));
        if (!page.error.empty()) {
            result.error = "snowflake: partition " + std::to_string(partition) +
                           ": " + page.error;
            return result;
        }
        if (!page.ok()) {
            result.error = snowflake::status_error(page);
            return result;
        }
        const auto body =
            nlohmann::json::parse(page.body, nullptr, /*allow_exceptions=*/false);
        if (!body.is_object()) {
            result.error = "snowflake: partition " + std::to_string(partition) +
                           " body is not a JSON object";
            return result;
        }
        snowflake::append_data(body, plan.columns, plan.column_types, max_rows,
                               result.rows);
        ++result.partitions_fetched;
    }
    return result;
}

PartitionFetchResult fetch_result_partitions(const ConnectorConfig& config,
                                             const StatementPlan& plan,
                                             const std::vector<std::size_t>& partitions,
                                             std::size_t max_rows,
                                             std::size_t max_in_flight,
                                             const AsyncHttpTransport& transport) {
    PartitionFetchResult result;
    if (partitions.empty()) return result;
    if (!transport) {
        result.error =
            "no asynchronous HTTP transport is installed in this build — the "
            "concurrent partition fetch cannot run";
        return result;
    }
    if (plan.handle.empty()) {
        result.error = "snowflake: the statement carried no handle to fetch by";
        return result;
    }

    const std::size_t in_flight =
        std::clamp<std::size_t>(max_in_flight, 1, kMaxPartitionsInFlight);

    // Waves of `in_flight` concurrent exchanges: enough overlap to saturate the
    // node's link, few enough sockets (and TLS handshakes) to stay civil.
    for (std::size_t start = 0; start < partitions.size(); start += in_flight) {
        if (result.rows.size() >= max_rows) break;
        const std::size_t stop = std::min(start + in_flight, partitions.size());

        std::vector<HttpRequest> wave;
        wave.reserve(stop - start);
        for (std::size_t index = start; index < stop; ++index) {
            wave.push_back(snowflake::partition_request(
                config, plan.handle, static_cast<long long>(partitions[index])));
        }

        const std::vector<HttpResponse> responses = runtime::block_on(
            [](AsyncHttpTransport exchange, std::vector<HttpRequest> requests)
                -> runtime::task<std::vector<HttpResponse>> {
                std::vector<runtime::io_task<HttpResponse>> in_flight_exchanges;
                in_flight_exchanges.reserve(requests.size());
                for (const HttpRequest& request : requests) {
                    in_flight_exchanges.push_back(exchange(request));
                }
                auto [error, gathered] =
                    co_await runtime::when_all(std::move(in_flight_exchanges));
                if (error) {
                    // A transport-level failure of the JOIN itself (not of one
                    // exchange, which reports through HttpResponse::error).
                    std::vector<HttpResponse> failed(requests.size());
                    for (HttpResponse& response : failed) {
                        response.error = error.message();
                    }
                    co_return failed;
                }
                co_return std::move(gathered);
            }(transport, std::move(wave)));

        for (std::size_t offset = 0; offset < responses.size(); ++offset) {
            const std::size_t partition = partitions[start + offset];
            const HttpResponse& response = responses[offset];
            if (!response.error.empty()) {
                result.error = "snowflake: partition " +
                               std::to_string(partition) + ": " + response.error;
                return result;
            }
            if (!response.ok()) {
                result.error = snowflake::status_error(response);
                return result;
            }
            const auto body = nlohmann::json::parse(response.body, nullptr,
                                                    /*allow_exceptions=*/false);
            if (!body.is_object()) {
                result.error = "snowflake: partition " +
                               std::to_string(partition) +
                               " body is not a JSON object";
                return result;
            }
            snowflake::append_data(body, plan.columns, plan.column_types,
                                   max_rows, result.rows);
            ++result.partitions_fetched;
        }
    }
    return result;
}

}  // namespace zetta::connect
