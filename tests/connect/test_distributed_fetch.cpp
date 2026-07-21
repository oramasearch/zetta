// The distributed connector fetch (#827, docs/data/connector-query.md): the
// deterministic row-balanced tiling of a result set across nodes, and the
// concurrent, bounded partition read that each node runs over its own slice.
//
// No network: the transport seam is filled with fakes on both shapes, so these
// exercise the real submit/tile/fetch logic and nothing else.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "zetta/connect/connector.hpp"
#include "zetta/connect/distributed_fetch.hpp"
#include "zetta/runtime.hpp"

namespace {

using zetta::connect::AsyncHttpTransport;
using zetta::connect::HttpRequest;
using zetta::connect::HttpResponse;
using zetta::connect::PartitionAssignment;

zetta::connect::ConnectorConfig config() {
    zetta::connect::ConnectorConfig configuration;
    configuration.type = "snowflake";
    configuration.account = "ab12345.us-east-1";
    configuration.token = "a-token";
    configuration.warehouse = "COMPUTE_WH";
    return configuration;
}

// --- the tiling ---------------------------------------------------------------

std::vector<std::string> nodes(std::size_t count) {
    std::vector<std::string> ids;
    for (std::size_t index = 0; index < count; ++index) {
        ids.push_back("node-" + std::to_string(index));
    }
    return ids;
}

TEST(PartitionAssignments, TilesEveryPartitionExactlyOnce) {
    const std::vector<std::uint64_t> rows = {900, 100, 700, 50, 640, 1200, 30};
    for (std::size_t node_count = 1; node_count <= 5; ++node_count) {
        const auto plan = zetta::connect::plan_partition_assignments(
            nodes(node_count), rows);
        ASSERT_EQ(plan.size(), node_count);

        std::multiset<std::size_t> seen;
        for (const PartitionAssignment& assignment : plan) {
            for (const std::size_t partition : assignment.partitions) {
                seen.insert(partition);
            }
        }
        // No loss, no duplication — the property the whole design rests on.
        ASSERT_EQ(seen.size(), rows.size()) << "node_count=" << node_count;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            EXPECT_EQ(seen.count(index), 1u)
                << "partition " << index << " with " << node_count << " nodes";
        }
    }
}

TEST(PartitionAssignments, BalancesByRowCountNotByIndex) {
    // Round-robin over indices would give node-0 {0,2} = 1000+1 and node-1
    // {1,3} = 1+1000; here the two big partitions must land on different nodes
    // and the total must come out even.
    const std::vector<std::uint64_t> rows = {1000, 1, 1, 1000};
    const auto plan = zetta::connect::plan_partition_assignments(nodes(2), rows);
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan[0].rows, plan[1].rows);
    EXPECT_EQ(plan[0].rows, 1001u);
}

TEST(PartitionAssignments, IsDeterministicSoNoPlanHasToTravel) {
    const std::vector<std::uint64_t> rows = {5, 9, 1, 9, 3, 7, 2, 8};
    const auto first = zetta::connect::plan_partition_assignments(nodes(3), rows);
    const auto second = zetta::connect::plan_partition_assignments(nodes(3), rows);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(first[index].node, second[index].node);
        EXPECT_EQ(first[index].partitions, second[index].partitions);
        EXPECT_EQ(first[index].rows, second[index].rows);
    }
}

TEST(PartitionAssignments, HandlesEmptyInputs) {
    EXPECT_TRUE(zetta::connect::plan_partition_assignments({}, {1, 2}).empty());
    const auto plan = zetta::connect::plan_partition_assignments(nodes(3), {});
    ASSERT_EQ(plan.size(), 3u);
    for (const PartitionAssignment& assignment : plan) {
        EXPECT_TRUE(assignment.partitions.empty());
        EXPECT_EQ(assignment.rows, 0u);
    }
}

TEST(PartitionAssignments, MoreNodesThanPartitionsLeavesSomeIdle) {
    const auto plan = zetta::connect::plan_partition_assignments(nodes(5), {10, 10});
    ASSERT_EQ(plan.size(), 5u);
    std::size_t assigned = 0;
    for (const PartitionAssignment& assignment : plan) {
        assigned += assignment.partitions.size();
    }
    EXPECT_EQ(assigned, 2u);
}

// --- a fake source ------------------------------------------------------------

// A fake Snowflake that answers the submit with `partition_count` partitions and
// serves each partition's rows off the handle. Records how many exchanges were
// in flight at once so the test can prove the fetch actually overlapped.
struct FakeSource {
    std::size_t partition_count = 4;
    std::size_t rows_per_partition = 3;
    std::atomic<int> in_flight{0};
    std::atomic<int> peak_in_flight{0};
    std::atomic<int> partition_requests{0};

    // A one-shot gate the async fake suspends on, so a whole wave is genuinely
    // in flight together before any exchange completes (a fake that returned
    // immediately would never overlap, and `peak_in_flight` would be a lie).
    // Set `release_at` to the wave size under test; later waves pass straight
    // through, which is enough to observe the peak and its bound.
    zetta::runtime::async_event first_wave;
    std::size_t release_at = 1;
    std::atomic<int> arrivals{0};

    nlohmann::json rows_for(std::size_t partition) const {
        nlohmann::json data = nlohmann::json::array();
        for (std::size_t index = 0; index < rows_per_partition; ++index) {
            const std::size_t ordinal = partition * rows_per_partition + index;
            data.push_back(nlohmann::json::array(
                {std::to_string(ordinal), "row-" + std::to_string(ordinal)}));
        }
        return data;
    }

    nlohmann::json metadata() const {
        nlohmann::json partitions = nlohmann::json::array();
        for (std::size_t index = 0; index < partition_count; ++index) {
            partitions.push_back({{"rowCount", rows_per_partition}});
        }
        return {{"rowType",
                 nlohmann::json::array({{{"name", "id"}, {"type", "fixed"}},
                                        {{"name", "label"}, {"type", "text"}}})},
                {"partitionInfo", partitions}};
    }

    HttpResponse answer(const HttpRequest& request) {
        HttpResponse response;
        response.status = 200;
        if (request.method == "POST") {
            response.body =
                nlohmann::json{{"statementHandle", "handle-1"},
                               {"resultSetMetaData", metadata()},
                               {"data", rows_for(0)}}
                    .dump();
            return response;
        }
        ++partition_requests;
        const std::size_t marker = request.url.find("partition=");
        const std::size_t partition =
            marker == std::string::npos
                ? 0
                : static_cast<std::size_t>(
                      std::stoul(request.url.substr(marker + 10)));
        response.body = nlohmann::json{{"data", rows_for(partition)}}.dump();
        return response;
    }
};

// The coroutine shape of the fake: it suspends on a shared event so every
// exchange in a wave is genuinely in flight together before any completes.
AsyncHttpTransport async_transport_over(FakeSource& source) {
    return [&source](const HttpRequest& request)
               -> zetta::runtime::io_task<HttpResponse> {
        const int now = ++source.in_flight;
        int peak = source.peak_in_flight.load();
        while (peak < now && !source.peak_in_flight.compare_exchange_weak(peak, now)) {
        }
        if (static_cast<std::size_t>(++source.arrivals) >= source.release_at) {
            source.first_wave.set();
        }
        [[maybe_unused]] const auto waited = co_await source.first_wave.wait();
        HttpResponse response = source.answer(request);
        --source.in_flight;
        co_return {{}, std::move(response)};
    };
}

zetta::connect::HttpTransport sync_transport_over(FakeSource& source) {
    return [&source](const HttpRequest& request) { return source.answer(request); };
}

TEST(DistributedFetch, SubmitReadsBackThePlanAndTheInlinePartition) {
    FakeSource source;
    source.partition_count = 4;
    const auto submitted = zetta::connect::submit_connector_statement(
        config(), "SELECT 1", 1000, sync_transport_over(source));
    ASSERT_TRUE(submitted.ok()) << submitted.error;
    EXPECT_EQ(submitted.plan.handle, "handle-1");
    EXPECT_EQ(submitted.plan.partition_rows.size(), 4u);
    EXPECT_EQ(submitted.plan.fetchable_partitions(), 3u);
    EXPECT_EQ(submitted.plan.columns, (std::vector<std::string>{"id", "label"}));
    // Partition 0 came inline with the submission — no second request for it.
    EXPECT_EQ(submitted.inline_rows.rows.size(), 3u);
    EXPECT_EQ(source.partition_requests.load(), 0);
    // …and typed by the declared rowType, not left as strings.
    EXPECT_TRUE(submitted.inline_rows.rows.front().at("id").is_number());
}

// A source that answers 202 (still executing) for the first `pending` reads of
// the handle, then completes. The poll between reads is a `runtime::delay`
// timer, not a blocking sleep (#832) — the whole submit is driven as a
// coroutine, so this also proves the coroutine shape actually runs.
struct SlowSource : FakeSource {
    std::atomic<int> pending{2};

    HttpResponse answer_slow(const HttpRequest& request) {
        HttpResponse response;
        if (request.method == "POST" || pending.fetch_sub(1) > 0) {
            response.status = 202;
            response.body =
                nlohmann::json{{"statementHandle", "handle-1"}}.dump();
            return response;
        }
        // Completed: the handle read now answers exactly like the submit would
        // have, metadata and inline partition 0 included.
        response.status = 200;
        response.body = nlohmann::json{{"statementHandle", "handle-1"},
                                       {"resultSetMetaData", metadata()},
                                       {"data", rows_for(0)}}
                            .dump();
        return response;
    }
};

TEST(DistributedFetch, PollsAnAsynchronouslyExecutingStatementWithoutSleeping) {
    SlowSource source;
    source.partition_count = 2;
    const auto started = std::chrono::steady_clock::now();
    const auto submitted = zetta::connect::submit_connector_statement(
        config(), "SELECT 1", 1000,
        [&source](const HttpRequest& request) {
            return source.answer_slow(request);
        });
    ASSERT_TRUE(submitted.ok()) << submitted.error;
    EXPECT_EQ(submitted.plan.handle, "handle-1");
    EXPECT_EQ(submitted.plan.partition_rows.size(), 2u);
    EXPECT_EQ(submitted.inline_rows.rows.size(), source.rows_per_partition);
    // Polls really elapsed on the timer (500ms apiece) rather than the statement
    // being read straight through.
    EXPECT_GE(std::chrono::steady_clock::now() - started,
              std::chrono::milliseconds(900));
}

TEST(DistributedFetch, FetchesASliceConcurrentlyAndExactlyOnce) {
    FakeSource source;
    source.partition_count = 9;
    const auto submitted = zetta::connect::submit_connector_statement(
        config(), "SELECT 1", 1000, sync_transport_over(source));
    ASSERT_TRUE(submitted.ok()) << submitted.error;

    const std::vector<std::size_t> slice = {1, 2, 3, 4, 5, 6, 7, 8};
    source.release_at = 4;  // the wave size below
    const auto fetched = zetta::connect::fetch_result_partitions(
        config(), submitted.plan, slice, /*max_rows=*/1000,
        /*max_in_flight=*/4, async_transport_over(source));
    ASSERT_TRUE(fetched.ok()) << fetched.error;
    EXPECT_EQ(fetched.partitions_fetched, slice.size());
    EXPECT_EQ(fetched.rows.size(), slice.size() * source.rows_per_partition);
    EXPECT_EQ(source.partition_requests.load(),
              static_cast<int>(slice.size()));  // each partition read once

    // Every id from the fetched partitions, exactly once.
    std::multiset<int> ids;
    for (const nlohmann::json& row : fetched.rows) ids.insert(row.at("id").get<int>());
    for (const std::size_t partition : slice) {
        for (std::size_t index = 0; index < source.rows_per_partition; ++index) {
            const int ordinal =
                static_cast<int>(partition * source.rows_per_partition + index);
            EXPECT_EQ(ids.count(ordinal), 1u) << "row " << ordinal;
        }
    }
    // The wave really overlapped — this is the whole point of the async seam.
    EXPECT_GT(source.peak_in_flight.load(), 1);
}

TEST(DistributedFetch, BoundsTheNumberOfExchangesInFlight) {
    FakeSource source;
    source.partition_count = 33;
    const auto submitted = zetta::connect::submit_connector_statement(
        config(), "SELECT 1", 100000, sync_transport_over(source));
    ASSERT_TRUE(submitted.ok()) << submitted.error;

    std::vector<std::size_t> slice;
    for (std::size_t index = 1; index < 33; ++index) slice.push_back(index);
    source.release_at = 3;  // the wave size below
    const auto fetched = zetta::connect::fetch_result_partitions(
        config(), submitted.plan, slice, /*max_rows=*/100000,
        /*max_in_flight=*/3, async_transport_over(source));
    ASSERT_TRUE(fetched.ok()) << fetched.error;
    EXPECT_EQ(fetched.partitions_fetched, slice.size());
    EXPECT_LE(source.peak_in_flight.load(), 3);
}

TEST(DistributedFetch, WithoutAnAsyncTransportItFailsCleanlyRatherThanCrashing) {
    FakeSource source;
    const auto submitted = zetta::connect::submit_connector_statement(
        config(), "SELECT 1", 1000, sync_transport_over(source));
    ASSERT_TRUE(submitted.ok()) << submitted.error;
    const auto fetched = zetta::connect::fetch_result_partitions(
        config(), submitted.plan, {1, 2}, 1000, 4, AsyncHttpTransport{});
    EXPECT_FALSE(fetched.ok());
    EXPECT_NE(fetched.error.find("asynchronous HTTP transport"),
              std::string::npos)
        << fetched.error;
}

// Tiling a result set across N nodes and fetching each slice must reconstruct
// the whole result set exactly once — the multi-node acceptance, driven here
// without a cluster so the property is asserted on the algorithm itself.
TEST(DistributedFetch, TheUnionOfEveryNodesSliceIsTheWholeResultExactlyOnce) {
    FakeSource source;
    source.partition_count = 11;
    source.rows_per_partition = 4;
    const auto submitted = zetta::connect::submit_connector_statement(
        config(), "SELECT 1", 100000, sync_transport_over(source));
    ASSERT_TRUE(submitted.ok()) << submitted.error;

    const auto plan = zetta::connect::plan_partition_assignments(
        nodes(4), submitted.plan.partition_rows);

    std::multiset<int> ids;
    // Partition 0 is held by whichever node the tiling gave it; its rows came
    // inline with the submission on the coordinating node.
    for (const nlohmann::json& row : submitted.inline_rows.rows) {
        ids.insert(row.at("id").get<int>());
    }
    for (const PartitionAssignment& assignment : plan) {
        std::vector<std::size_t> slice = assignment.partitions;
        slice.erase(std::remove(slice.begin(), slice.end(), std::size_t{0}),
                    slice.end());
        if (slice.empty()) continue;
        const auto fetched = zetta::connect::fetch_result_partitions(
            config(), submitted.plan, slice, 100000,
            zetta::connect::kDefaultPartitionsInFlight,
            async_transport_over(source));
        ASSERT_TRUE(fetched.ok()) << fetched.error;
        for (const nlohmann::json& row : fetched.rows) {
            ids.insert(row.at("id").get<int>());
        }
    }

    const std::size_t total = source.partition_count * source.rows_per_partition;
    ASSERT_EQ(ids.size(), total);
    for (int ordinal = 0; ordinal < static_cast<int>(total); ++ordinal) {
        EXPECT_EQ(ids.count(ordinal), 1u) << "row " << ordinal;
    }
    // …and each partition was read from the source exactly once across the
    // whole cluster (partition 0 never re-read — it arrived inline).
    EXPECT_EQ(source.partition_requests.load(),
              static_cast<int>(source.partition_count - 1));
}

// run_connector_query keeps working end to end over the concurrent path.
TEST(DistributedFetch, RunConnectorQueryUsesTheConcurrentPathWhenGivenOne) {
    FakeSource source;
    source.partition_count = 6;
    source.release_at = zetta::connect::kDefaultPartitionsInFlight;
    const auto result = zetta::connect::run_connector_query(
        config(), "SELECT id, label FROM t", 1000, sync_transport_over(source),
        async_transport_over(source));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.data.rows.size(), 6u * source.rows_per_partition);
    EXPECT_GT(source.peak_in_flight.load(), 1);
}

}  // namespace
