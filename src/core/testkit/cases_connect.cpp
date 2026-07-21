#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cases.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/distributed_ingest.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/service.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/storage/lazy_row.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/util/uuid.hpp"

// Source-connector cases (#827, #833) — the two halves of "pull a warehouse
// query into the cluster", both run on a real multi-node loopback cluster and
// both with an **injected fake transport**: no network, no credentials, no
// warehouse anywhere in the process.
//
//   * `cluster.connections` — the credential surface. A saved connection is
//     replicate-to-all (every node holds the token, which is what lets a node
//     fetch its own stripe), a write statement is refused before any request is
//     built, the token appears on no read surface, and a query returns typed
//     cells rather than a table of strings.
//   * `cluster.distributed_import` — the co-launched ingest. Every source chunk
//     is fetched exactly once cluster-wide, each node owns the partitions it
//     pulled, no row re-replicates through the CRDT, and the dataset reads back
//     complete through the scatter-gather SELECT.
//
// Both are timing-bound: convergence polls against a seconds-long deadline,
// never a fixed iteration count. See docs/data/connector-query.md and
// docs/data/distributed-connector-ingest.md.
namespace zetta::testkit {
namespace {

using zetta::connect::AsyncHttpTransport;
using zetta::connect::ConnectorConfig;
using zetta::connect::ConnectorResult;
using zetta::connect::HttpRequest;
using zetta::connect::HttpResponse;
using zetta::connect::HttpTransport;
using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

std::size_t node_option(const TestOptions& options, long fallback) {
    return static_cast<std::size_t>(
        std::min<long>(6, std::max<long>(2, options.get_int("nodes", fallback))));
}

// Patient: poll `ready` against a seconds-long deadline (never a fixed count).
template <class Predicate>
bool wait_until(Predicate ready, std::chrono::seconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return ready();
}

// The connector settings every case here uses. The credential is unique per run
// so a leak assertion can never pass because it matched a stale string.
ConnectorConfig warehouse_config(const std::string& token) {
    ConnectorConfig config;
    config.type = "snowflake";
    config.account = "ab12345.us-east-1";
    config.token = token;
    config.warehouse = "COMPUTE_WH";
    config.database = "ANALYTICS";
    config.schema = "PUBLIC";
    return config;
}

// ---------------------------------------------------------------------------
// cluster.connections
// ---------------------------------------------------------------------------

// A canned Snowflake SQL API v2 body with three deliberately different cell
// types — text, a number, a boolean — plus a null, so "the cells are typed" is
// checkable rather than asserted by eye.
std::string typed_result_body() {
    return Json{{"statementHandle", "handle-1"},
                {"resultSetMetaData",
                 Json{{"rowType",
                       Json::array({{{"name", "ACCOUNT_ID"}, {"type", "text"}},
                                    {{"name", "MRR"}, {"type", "fixed"}},
                                    {{"name", "CHURNED"}, {"type", "boolean"}}})},
                      {"partitionInfo", Json::array({{{"rowCount", 2}}})}}},
                {"data", Json::array({Json::array({"acct_globex", "1200", "false"}),
                                      Json::array({"acct_initech", nullptr, nullptr})})}}
        .dump();
}

// The saved-connection surface over a real cluster: it replicates, it refuses to
// write, it never shows the credential, and it returns typed rows.
TestResult run_connections(const TestOptions& options, TestContext& context) {
    const std::size_t nodes = node_option(options, 3);
    const std::string name = "warehouse";
    // Unique per run: nothing else in the process can coincidentally hold it.
    const std::string secret = "pat-" + zetta::util::new_uuid();

    LoopbackOptions wiring;
    wiring.replicated = true;  // the CRDT document behind every node
    context.emit("bringing up a " + std::to_string(nodes) +
                 "-node replicated loopback cluster");
    LoopbackCluster cluster(nodes, wiring);
    if (!cluster.connected()) {
        return TestResult::fail("cluster did not fully connect within the bring-up window");
    }
    for (std::size_t index = 0; index < cluster.size(); ++index) {
        cluster.announce_member(index);
    }

    // --- 1. the connection round-trips to every node --------------------------
    context.emit("saving connection '" + name + "' on node 0");
    const std::string rejection =
        zetta::connect::save_connection(cluster.node(0), name, warehouse_config(secret));
    if (!rejection.empty()) return TestResult::fail("save_connection: " + rejection);

    context.emit("waiting for it to reach the other " + std::to_string(nodes - 1) +
                 " nodes");
    const bool everywhere = wait_until(
        [&] {
            for (std::size_t index = 0; index < cluster.size(); ++index) {
                cluster.node(index).tick();
                const auto found = zetta::connect::find_connection(cluster.node(index), name);
                // The resolution path is the one caller allowed to see the
                // credential — and every node needs it, because every node
                // fetches its own stripe with its own copy.
                if (!found || found->config.token != secret) return false;
            }
            return true;
        },
        std::chrono::seconds(30));
    if (!everywhere) {
        return TestResult::fail("the saved connection did not reach every node with its token");
    }
    context.emit("resolved on " + std::to_string(nodes) + "/" + std::to_string(nodes) +
                 " nodes, credential intact");

    // The store is an ordinary replicate-to-all table — that is *why* it lands
    // everywhere, rather than a bespoke fan-out nobody else would inherit.
    const auto store_database = cluster.node(1).find_database(zetta::connect::kConnectionDatabase);
    if (!store_database) return TestResult::fail("node 1 has no connection database");
    const auto store_table = store_database->find_table(zetta::connect::kConnectionTable);
    if (!store_table) return TestResult::fail("node 1 has no connection table");
    if (!store_table->replication_strategy().is_default()) {
        return TestResult::fail("the connection store is not replicate-to-all");
    }

    // --- 2. the read-only guard, before any request is built ------------------
    context.emit("checking the read-only guard refuses writes before egress");
    if (!zetta::connect::read_only_query_rejection("SELECT * FROM churn").empty()) {
        return TestResult::fail("the guard refused a plain SELECT");
    }
    const std::vector<std::string> writes = {
        "DROP TABLE accounts",
        "INSERT INTO accounts VALUES (1)",
        "SELECT 1; DROP TABLE accounts",  // a write smuggled behind a read
    };
    std::size_t refused = 0;
    for (const std::string& sql : writes) {
        std::size_t calls = 0;
        const HttpTransport counting = [&calls](const HttpRequest&) {
            ++calls;
            HttpResponse response;
            response.status = 200;
            response.body = "{}";
            return response;
        };
        const ConnectorResult result = zetta::connect::run_connector_query(
            warehouse_config(secret), sql, 100, counting);
        if (result.ok()) {
            return TestResult::fail("the guard accepted a write statement: " + sql);
        }
        if (calls != 0) {
            return TestResult::fail("a write statement reached the transport: " + sql);
        }
        ++refused;
    }
    context.emit(std::to_string(refused) +
                 " write statements refused; the transport was never called");

    // --- 3. the credential is on no read surface, on any node -----------------
    context.emit("checking every read surface for the credential");
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    for (std::size_t index = 0; index < cluster.size(); ++index) {
        Node& node = cluster.node(index);
        const std::string where = "node " + std::to_string(index) + ": ";

        const auto found = zetta::connect::find_connection(node, name);
        if (!found) return TestResult::fail(where + "lost the connection");
        if (found->to_redacted_json().dump().find(secret) != std::string::npos) {
            return TestResult::fail(where + "to_redacted_json leaked the token");
        }
        if (found->config.to_json().dump().find(secret) != std::string::npos) {
            return TestResult::fail(where + "ConnectorConfig::to_json leaked the token");
        }
        // …and the one serialization that DOES carry it is the store's own.
        if (found->config.to_storage_json().dump().find(secret) == std::string::npos) {
            return TestResult::fail(where + "the store's own serialization lost the token");
        }

        zetta::query::QueryService service(node);
        const auto view = service.run("SELECT * FROM system.connections;", "");
        if (!view.ok()) return TestResult::fail(where + "system.connections: " + view.error);
        if (view.rows.size() != 1) {
            return TestResult::fail(where + "system.connections had " +
                                    std::to_string(view.rows.size()) + " rows, expected 1");
        }
        if (view.rows.front().dump().find(secret) != std::string::npos) {
            return TestResult::fail(where + "system.connections leaked the token");
        }
        if (view.rows.front().value("name", std::string{}) != name) {
            return TestResult::fail(where + "system.connections named the wrong connection");
        }

        zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
        zetta::api::ApiContext api{node, nullptr, "", trace, &node.span_recorder()};
        const zetta::api::ApiResult listed =
            registry.call("list_connections", Json::object(), api);
        if (!listed.ok) {
            return TestResult::fail(where + "list_connections: " + listed.error.message);
        }
        const std::string output = listed.value.dump();
        if (output.find(secret) != std::string::npos) {
            return TestResult::fail(where + "the list_connections capability leaked the token");
        }
        if (output.find(name) == std::string::npos) {
            return TestResult::fail(where + "the list_connections capability lost the connection");
        }
    }
    context.emit("token absent from to_json, system.connections and list_connections "
                 "on all " + std::to_string(nodes) + " nodes");

    // --- 4. a query returns typed cells ---------------------------------------
    context.emit("running a query through the fake transport");
    std::size_t requests = 0;
    const HttpTransport canned = [&requests](const HttpRequest& request) {
        ++requests;
        HttpResponse response;
        response.status = 200;
        response.body = request.method == "POST" ? typed_result_body() : "{}";
        return response;
    };
    const ConnectorResult query = zetta::connect::run_connector_query(
        warehouse_config(secret), "SELECT ACCOUNT_ID, MRR, CHURNED FROM churn", 100, canned);
    if (!query.ok()) return TestResult::fail("the read-only query failed: " + query.error);
    if (query.data.columns.size() != 3 || query.data.rows.size() != 2) {
        return TestResult::fail("the query returned the wrong shape");
    }
    const Json& first = query.data.rows.front();
    const Json& second = query.data.rows.back();
    if (!first.at("ACCOUNT_ID").is_string()) {
        return TestResult::fail("a text cell did not come back as a string");
    }
    if (!first.at("MRR").is_number() || first.at("MRR") != 1200) {
        return TestResult::fail("a numeric cell came back as a string");
    }
    if (!first.at("CHURNED").is_boolean() || first.at("CHURNED") != false) {
        return TestResult::fail("a boolean cell came back as a string");
    }
    if (!second.at("MRR").is_null() || !second.at("CHURNED").is_null()) {
        return TestResult::fail("a NULL cell did not come back as JSON null");
    }

    TestResult result = TestResult::pass(
        "a saved connection reached every node with its credential, writes were refused "
        "before egress, no read surface carried the token, and the cells came back typed");
    result.metric("nodes", std::to_string(nodes))
        .metric("resolved on", std::to_string(nodes) + "/" + std::to_string(nodes) + " nodes")
        .metric("write statements refused", std::to_string(refused))
        .metric("transport calls during refusal", "0")
        .metric("read surfaces checked", std::to_string(nodes * 4) + " (4 per node)")
        .metric("query requests", std::to_string(requests))
        .metric("typed columns", std::to_string(query.data.columns.size()))
        .metric("rows", std::to_string(query.data.rows.size()));
    return result;
}

// ---------------------------------------------------------------------------
// cluster.distributed_import
// ---------------------------------------------------------------------------

// A fake warehouse whose result set is `chunks` independently-fetchable
// partitions. It records, per calling THREAD, which chunks that thread read —
// which is what makes "who fetched what" observable across the cluster: the
// initiator fetches on the case thread and each peer on its own tick thread.
struct FakeWarehouse {
    FakeWarehouse(std::size_t chunk_count, std::size_t rows_per_chunk)
        : chunks(chunk_count), rows_per_chunk(rows_per_chunk) {}

    std::size_t chunks;
    std::size_t rows_per_chunk;

    std::mutex mutex;
    std::map<std::size_t, int> reads;  // chunk -> times read
    std::map<std::thread::id, std::set<std::size_t>> by_thread;
    std::size_t bytes = 0;
    std::atomic<int> submits{0};

    std::size_t total_rows() const { return chunks * rows_per_chunk; }

    // Row `ordinal` of chunk `chunk`: a globally unique id, the chunk it came
    // from, and a value to aggregate (amount = id * 3).
    Json wire_rows(std::size_t chunk) const {
        Json data = Json::array();
        for (std::size_t index = 0; index < rows_per_chunk; ++index) {
            const std::size_t ordinal = chunk * rows_per_chunk + index;
            data.push_back(Json::array({std::to_string(ordinal), std::to_string(chunk),
                                        std::to_string(ordinal * 3)}));
        }
        return data;
    }

    Json metadata() const {
        Json partitions = Json::array();
        for (std::size_t index = 0; index < chunks; ++index) {
            partitions.push_back({{"rowCount", rows_per_chunk}});
        }
        return {{"rowType", Json::array({{{"name", "id"}, {"type", "fixed"}},
                                         {{"name", "chunk"}, {"type", "fixed"}},
                                         {{"name", "amount"}, {"type", "fixed"}}})},
                {"partitionInfo", partitions}};
    }

    HttpResponse answer(const HttpRequest& request) {
        HttpResponse response;
        response.status = 200;
        if (request.method == "POST") {
            ++submits;
            response.body = Json{{"statementHandle", "handle-1"},
                                 {"resultSetMetaData", metadata()},
                                 {"data", wire_rows(0)}}
                                .dump();
            const std::lock_guard<std::mutex> lock(mutex);
            bytes += response.body.size();
            return response;
        }
        const std::size_t marker = request.url.find("partition=");
        const std::size_t chunk =
            marker == std::string::npos
                ? 0
                : static_cast<std::size_t>(std::stoul(request.url.substr(marker + 10)));
        response.body = Json{{"data", wire_rows(chunk)}}.dump();
        const std::lock_guard<std::mutex> lock(mutex);
        ++reads[chunk];
        by_thread[std::this_thread::get_id()].insert(chunk);
        bytes += response.body.size();
        return response;
    }

    std::set<std::size_t> chunks_read_by(std::thread::id thread) {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto found = by_thread.find(thread);
        return found == by_thread.end() ? std::set<std::size_t>{} : found->second;
    }
};

// Installs a transport as the process-wide default for the life of the scope and
// **restores whatever was there before** — a case runs inside a live app, which
// may well have the native client installed; uninstalling it would leave the app
// unable to reach a source afterwards.
class InstalledTransport {
public:
    explicit InstalledTransport(HttpTransport transport)
        : previous_(zetta::connect::default_http_transport()),
          previous_async_(zetta::connect::default_async_http_transport()) {
        // No async half: a peer takes the documented serial degrade, which is
        // what makes each node's reads attributable to its own tick thread.
        zetta::connect::set_default_http_transport(std::move(transport), {});
    }
    ~InstalledTransport() {
        zetta::connect::set_default_http_transport(previous_, previous_async_);
    }
    InstalledTransport(const InstalledTransport&) = delete;
    InstalledTransport& operator=(const InstalledTransport&) = delete;

private:
    HttpTransport previous_;
    AsyncHttpTransport previous_async_;
};

// A node loop, exactly as a live process runs one: tick drains the run.launch
// queue and executes the co-fetch on this owner thread.
class TickLoop {
public:
    explicit TickLoop(Node& node)
        : thread_([this, &node] {
              thread_id_ = std::this_thread::get_id();
              started_.store(true);
              while (!stop_.load()) {
                  node.tick();
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
          }) {
        while (!started_.load()) std::this_thread::yield();
    }
    ~TickLoop() { stop(); }
    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }
    std::thread::id thread_id() const { return thread_id_; }

private:
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};
    std::thread::id thread_id_;
    std::thread thread_;
};

// The chunks a node holds locally — its staging copy of exactly the tile it
// fetched, which is what write_partitioned serialized into the bodies it owns.
std::set<std::size_t> local_chunks(Node& node, const std::string& table_name,
                                   std::size_t& rows) {
    std::set<std::size_t> chunks;
    rows = 0;
    const auto database = node.find_database("datasets");
    if (!database) return chunks;
    const auto table = database->find_table(table_name);
    if (!table) return chunks;
    const zetta::storage::StorageEngine& store = table->store();
    rows = store.size();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const zetta::storage::ParquetLazyPtr row = store.at(index);
        if (!row || !row->is_object()) continue;
        chunks.insert(row->at("chunk").get<std::size_t>());
    }
    return chunks;
}

// Every `row:` key in a node's replicated document that belongs to `table_id`.
// A partition-only table must produce none: the rows are on disk where they were
// pulled and never on the replication plane.
std::size_t replicated_row_entries(const Json& document, const std::string& table_id) {
    std::size_t rows = 0;
    for (const auto& [key, value] : document.items()) {
        if (!key.starts_with("row:") || !value.is_object()) continue;
        if (value.value("tbl", std::string{}) == table_id) ++rows;
    }
    return rows;
}

// The co-launched connector ingest over a real cluster: fetch once, land where
// you pulled, replicate no rows, read back whole.
TestResult run_distributed_import(const TestOptions& options, TestContext& context) {
    const std::size_t nodes = node_option(options, 3);
    const std::size_t rows_per_chunk =
        static_cast<std::size_t>(std::min<long>(2000, std::max<long>(1,
            options.get_int("rows", 6))));
    // Chunk 0 arrives inline with the submission, so there must be at least one
    // fetchable chunk per node for the fan-out to mean anything.
    const std::size_t requested_chunks =
        static_cast<std::size_t>(std::min<long>(256, std::max<long>(2,
            options.get_int("chunks", 9))));
    const std::size_t chunks = std::max(requested_chunks, nodes + 1);
    if (chunks != requested_chunks) {
        context.emit("raised the chunk count to " + std::to_string(chunks) +
                     " so every node has a fetchable chunk");
    }
    const std::size_t total_rows = chunks * rows_per_chunk;
    const std::string secret = "pat-" + zetta::util::new_uuid();

    FakeWarehouse source(chunks, rows_per_chunk);
    const InstalledTransport installed(
        [&source](const HttpRequest& request) { return source.answer(request); });

    LoopbackOptions wiring;
    wiring.attached = true;  // the run.launch listener the ingest is dispatched by
    context.emit("bringing up a " + std::to_string(nodes) +
                 "-node cluster wired the way a live process is");
    LoopbackCluster cluster(nodes, wiring);
    if (!cluster.connected()) {
        return TestResult::fail("cluster did not fully connect within the bring-up window");
    }

    // The credential is saved ONCE and replicates to every node — that is what
    // lets a peer present it itself, so nothing secret rides the launch.
    if (const std::string rejection = zetta::connect::save_connection(
            cluster.node(0), "warehouse", warehouse_config(secret));
        !rejection.empty()) {
        return TestResult::fail("save_connection: " + rejection);
    }
    if (!wait_until(
            [&] {
                for (std::size_t index = 0; index < cluster.size(); ++index) {
                    cluster.node(index).tick();
                    if (!zetta::connect::find_connection(cluster.node(index), "warehouse")) {
                        return false;
                    }
                }
                return true;
            },
            std::chrono::seconds(30))) {
        return TestResult::fail("the saved connection did not reach every node");
    }

    // The peers run their own node loops; the initiator submits from this thread
    // (so the initiator's tile is attributable to this thread).
    std::vector<std::unique_ptr<TickLoop>> peers;
    for (std::size_t index = 1; index < cluster.size(); ++index) {
        peers.push_back(std::make_unique<TickLoop>(cluster.node(index)));
    }

    context.emit("importing " + std::to_string(total_rows) + " rows in " +
                 std::to_string(chunks) + " source chunks across the cluster");
    zetta::connect::DistributedIngestOptions ingest;
    ingest.connection = "warehouse";
    ingest.query = "SELECT id, chunk, amount FROM source";
    ingest.table_name = "cached";
    ingest.partition_count = static_cast<std::uint32_t>(nodes * 2);
    ingest.replication_factor = 1;  // a result set is a cache
    ingest.coordination_timeout = std::chrono::seconds(60);

    const zetta::connect::DistributedIngestResult imported =
        zetta::connect::run_distributed_connector_ingest(
            cluster.node(0), warehouse_config(secret), ingest);
    if (!imported.ok()) return TestResult::fail("the ingest failed: " + imported.error);
    if (!imported.coordinated) {
        return TestResult::fail("the ingest was not co-launched — that is a parallel "
                                "loop on one node, not a distributed import");
    }
    if (imported.participants.size() != nodes) {
        return TestResult::fail("only " + std::to_string(imported.participants.size()) +
                                " of " + std::to_string(nodes) + " nodes participated");
    }
    if (!imported.table) return TestResult::fail("the ingest reported no table");
    const std::string table_name = imported.table->name();
    const std::string table_id = imported.table->id();

    // --- exactly once, cluster-wide, with per-node attribution ----------------
    if (source.submits.load() != 1) {
        return TestResult::fail("the statement was submitted " +
                                std::to_string(source.submits.load()) +
                                " times, not once for the whole cluster");
    }
    if (source.reads[0] != 0) {
        return TestResult::fail("chunk 0 arrived inline and was fetched again");
    }
    for (std::size_t chunk = 1; chunk < chunks; ++chunk) {
        if (source.reads[chunk] != 1) {
            return TestResult::fail("chunk " + std::to_string(chunk) + " was read " +
                                    std::to_string(source.reads[chunk]) +
                                    " times, expected exactly once");
        }
    }
    std::vector<std::set<std::size_t>> pulled;
    pulled.push_back(source.chunks_read_by(std::this_thread::get_id()));
    for (const auto& peer : peers) pulled.push_back(source.chunks_read_by(peer->thread_id()));
    std::set<std::size_t> union_of_tiles;
    std::size_t attributed = 0;
    std::size_t fetching_nodes = 0;
    for (const std::set<std::size_t>& tile : pulled) {
        attributed += tile.size();
        union_of_tiles.insert(tile.begin(), tile.end());
        if (!tile.empty()) ++fetching_nodes;
    }
    if (attributed != chunks - 1 || union_of_tiles.size() != chunks - 1) {
        return TestResult::fail("the per-node tiles overlap or do not cover the result set");
    }
    if (fetching_nodes < 2) {
        return TestResult::fail("only one node fetched — that is a parallel loop, not a "
                                "distributed import");
    }
    std::string per_node;
    for (std::size_t index = 0; index < pulled.size(); ++index) {
        if (index) per_node += " / ";
        per_node += std::to_string(pulled[index].size());
    }
    context.emit("chunks pulled per node: " + per_node + " (chunk 0 came inline)");

    // --- the dataset reads back complete and correct --------------------------
    // Patient, and with the peers still ticking: each participant's catalog
    // entries reach the initiator on the next gossip round.
    const std::string select = "SELECT * FROM datasets." + table_name + ";";
    zetta::query::QueryService service(cluster.node(0));
    zetta::query::QueryService::Outcome all;
    if (!wait_until(
            [&] {
                cluster.node(0).tick();  // the peers tick on their own loops
                all = service.run(select, "");
                return all.ok() && all.rows.size() == total_rows;
            },
            std::chrono::seconds(90))) {
        return TestResult::fail("the scatter-gather read never saw the whole dataset: " +
                                (all.error.empty() ? std::to_string(all.rows.size()) +
                                                         " of " + std::to_string(total_rows) +
                                                         " rows"
                                                   : all.error));
    }
    std::set<std::int64_t> ids;
    std::int64_t total_amount = 0;
    for (const Json& row : all.rows) {
        ids.insert(row.at("id").get<std::int64_t>());
        total_amount += row.at("amount").get<std::int64_t>();
    }
    if (ids.size() != total_rows) return TestResult::fail("a row came back twice");
    std::int64_t expected_amount = 0;
    for (std::size_t ordinal = 0; ordinal < total_rows; ++ordinal) {
        expected_amount += static_cast<std::int64_t>(ordinal) * 3;
    }
    if (total_amount != expected_amount) {
        return TestResult::fail("the gathered rows do not match the source");
    }
    context.emit("scatter-gather returned all " + std::to_string(total_rows) +
                 " rows, each exactly once");

    // The peers' loops stop here: from now on this thread is the only one
    // ticking, so their node trees (owner-thread-only) are safe to read.
    for (auto& peer : peers) peer->stop();

    // --- land where you pulled -------------------------------------------------
    // Each node holds exactly the rows of the chunks IT fetched (the initiator
    // additionally holds chunk 0, which came inline with its submission), and
    // every catalog entry it wrote names that node as its single owner.
    std::uint64_t catalogued = 0;
    std::size_t partitions_written = 0;
    for (std::size_t index = 0; index < cluster.size(); ++index) {
        std::set<std::size_t> expected = pulled[index];
        if (index == 0) expected.insert(0);
        std::size_t held_rows = 0;
        const std::set<std::size_t> held =
            local_chunks(cluster.node(index), table_name, held_rows);
        if (held != expected) {
            return TestResult::fail("node " + std::to_string(index) +
                                    " holds chunks it did not pull");
        }
        if (held_rows != expected.size() * rows_per_chunk) {
            return TestResult::fail("node " + std::to_string(index) +
                                    " landed the wrong number of rows");
        }
        std::uint64_t owned = 0;
        for (const auto& partition : cluster.node(index).dataset_partitions()) {
            if (!partition || partition->table() != table_id) continue;
            if (partition->owner_nodes().size() != 1) {
                return TestResult::fail("a cached result set must be RF=1");
            }
            if (std::find(imported.participants.begin(), imported.participants.end(),
                          partition->owner_nodes().front()) == imported.participants.end()) {
                return TestResult::fail("a partition is owned by a node that never fetched");
            }
            if (partition->owner_nodes().front() != cluster.node_id(index)) continue;
            if (partition->stats().value("placement", std::string{}) != "fetch_affinity") {
                return TestResult::fail("a partition was re-placed — a flexible source "
                                        "needs no rebalance");
            }
            owned += partition->row_range().end - partition->row_range().start;
            ++partitions_written;
        }
        if (owned != held_rows) {
            return TestResult::fail("node " + std::to_string(index) +
                                    " owns a different number of rows than it landed");
        }
        if (owned == 0) return TestResult::fail("node " + std::to_string(index) + " owns nothing");
        catalogued += owned;
    }
    if (catalogued != total_rows) {
        return TestResult::fail("the participants together own " + std::to_string(catalogued) +
                                " rows, not the whole result set");
    }
    context.emit("every partition is owned by the node that pulled it: " +
                 std::to_string(partitions_written) + " partitions, " +
                 std::to_string(catalogued) + " rows");

    // --- nothing re-replicated -------------------------------------------------
    // The table is partition-only, so no node's replicated document holds a
    // `row:` entry for it: the rows crossed the wire exactly once, from the
    // source to the node that will own them.
    for (std::size_t index = 0; index < cluster.size(); ++index) {
        const std::size_t replicated =
            replicated_row_entries(cluster.replicated_document(index), table_id);
        if (replicated != 0) {
            return TestResult::fail("node " + std::to_string(index) + " replicated " +
                                    std::to_string(replicated) +
                                    " rows of a partition-only table");
        }
    }
    context.emit("zero row: entries in every node's replicated document");

    TestResult result = TestResult::pass(
        "every source chunk was fetched exactly once cluster-wide, each node owns the "
        "partitions it pulled, no row re-replicated, and the dataset reads back whole");
    result.metric("nodes", std::to_string(nodes))
        .metric("source chunks", std::to_string(chunks))
        .metric("chunks per node", per_node + " (+1 inline on node 0)")
        .metric("fetching nodes", std::to_string(fetching_nodes))
        .metric("rows", std::to_string(total_rows))
        .metric("partitions written", std::to_string(partitions_written))
        .metric("planned partitions", std::to_string(imported.plan.partition_count))
        .metric("bytes off the source", std::to_string(source.bytes))
        .metric("replicated row entries", "0")
        .metric("statement submissions", std::to_string(source.submits.load()));
    return result;
}

// A participant that never does the work fails the WHOLE ingest — the all-or-
// nothing property (#840).
//
// The obvious way to model a dead participant is to wedge a node, and that would
// indeed be a poor thing to do from a panel someone is running. It is also
// unnecessary. This case owns its cluster: `LoopbackCluster` stands up its own
// nodes in their own temp directories and tears them all down on destruction, and
// the host process's live node is never touched. Within that, "dead" needs no
// fault injection at all — a node participates by running a tick loop, so a node
// that is simply never given one is a node that never drains the launch, never
// fetches its tile and never reports in. Nothing is broken, so nothing needs
// healing; the mechanism is the absence of a thread, and it is exactly what the
// L1 test does (tests/connect/test_distributed_ingest.cpp).
TestResult run_import_participant_loss(const TestOptions& options,
                                       TestContext& context) {
    const std::size_t nodes = node_option(options, 3);
    const std::size_t chunks = nodes + 1;
    const std::size_t rows_per_chunk = 6;
    const std::string secret = "pat-" + zetta::util::new_uuid();

    FakeWarehouse source(chunks, rows_per_chunk);
    const InstalledTransport installed(
        [&source](const HttpRequest& request) { return source.answer(request); });

    LoopbackOptions wiring;
    wiring.attached = true;
    context.emit("bringing up a " + std::to_string(nodes) +
                 "-node cluster wired the way a live process is");
    LoopbackCluster cluster(nodes, wiring);
    if (!cluster.connected()) {
        return TestResult::fail("cluster did not fully connect within the bring-up window");
    }

    if (const std::string rejection = zetta::connect::save_connection(
            cluster.node(0), "warehouse", warehouse_config(secret));
        !rejection.empty()) {
        return TestResult::fail("save_connection: " + rejection);
    }
    if (!wait_until(
            [&] {
                for (std::size_t index = 0; index < cluster.size(); ++index) {
                    cluster.node(index).tick();
                    if (!zetta::connect::find_connection(cluster.node(index), "warehouse")) {
                        return false;
                    }
                }
                return true;
            },
            std::chrono::seconds(30))) {
        return TestResult::fail("the saved connection did not reach every node");
    }

    // Every peer but the last gets a loop. The last is a full member of the
    // roster — connected, counted, a collective participant — and does no work.
    std::vector<std::unique_ptr<TickLoop>> peers;
    for (std::size_t index = 1; index + 1 < cluster.size(); ++index) {
        peers.push_back(std::make_unique<TickLoop>(cluster.node(index)));
    }
    context.emit("node " + std::to_string(cluster.size() - 1) +
                 " stays in the roster but never drains the launch");

    zetta::connect::DistributedIngestOptions ingest;
    ingest.connection = "warehouse";
    ingest.query = "SELECT id, chunk, amount FROM source";
    ingest.table_name = "cached";
    ingest.partition_count = static_cast<std::uint32_t>(nodes * 2);
    ingest.replication_factor = 1;
    // Short, because the expected outcome is the timeout: the missing node's
    // contribution to the agreement never arrives.
    ingest.coordination_timeout = std::chrono::seconds(8);

    const zetta::connect::DistributedIngestResult imported =
        zetta::connect::run_distributed_connector_ingest(
            cluster.node(0), warehouse_config(secret), ingest);

    if (imported.ok()) {
        return TestResult::fail("the ingest succeeded with a participant that never "
                                "fetched — a partial result set was reported as a "
                                "complete dataset");
    }
    if (imported.error.find("re-run the ingest") == std::string::npos) {
        return TestResult::fail("the failure does not tell the caller what to do: " +
                                imported.error);
    }
    if (imported.table) {
        return TestResult::fail("a failed ingest reported a dataset");
    }
    context.emit("the ingest failed as it must: " + imported.error);

    // Join the loops before reading the nodes' trees: a peer's own copy of the
    // launch must have finished for that read to mean anything.
    peers.clear();

    // The node that never ran landed nothing in the first place — it never fetched
    // a tile, so there is nothing for it to have kept.
    {
        const std::size_t idle = cluster.size() - 1;
        const auto database = cluster.node(idle).find_database("datasets");
        if (database && database->find_table("cached")) {
            return TestResult::fail("the node that never drained the launch somehow "
                                    "landed a tile");
        }
    }

    // And nothing partial survives on the nodes that DID run. Each participant
    // discards what it had landed once the agreement comes up short.
    //
    // Patiently: the idle node is exactly the node that has not been reconciling,
    // so it learns of the drop only from the ticks below — the cleanup has to
    // reach it, and asserting on one tick would be asserting on a race rather
    // than on the property.
    // Only the nodes that were RUNNING are ticked. Ticking the idle one would be
    // modelling something else: a node that was merely paused, which on waking
    // still drains the launch it queued and re-adopts the table AFTER the cleanup
    // ran. That is a real behaviour and a real race — a late launch beating a
    // completed cleanup — but it is the behaviour of a node that came back, not
    // of a node that died, and this case is about the latter.
    const auto discarded_everywhere = [&] {
        for (std::size_t index = 0; index + 1 < cluster.size(); ++index) {
            cluster.node(index).tick();
        }
        for (std::size_t index = 0; index < cluster.size(); ++index) {
            const auto database = cluster.node(index).find_database("datasets");
            if (database && database->find_table("cached")) return false;
            for (const auto& summary : cluster.node(index).datasets()) {
                if (summary.name == "cached") return false;
            }
        }
        return true;
    };
    if (!wait_until(discarded_everywhere, std::chrono::seconds(30))) {
        for (std::size_t index = 0; index < cluster.size(); ++index) {
            const auto database = cluster.node(index).find_database("datasets");
            if (database && database->find_table("cached")) {
                return TestResult::fail("node " + std::to_string(index) +
                                        " kept the table of a failed ingest");
            }
            for (const auto& summary : cluster.node(index).datasets()) {
                if (summary.name == "cached") {
                    return TestResult::fail("node " + std::to_string(index) +
                                            " still lists 'cached' in system.datasets");
                }
            }
        }
        return TestResult::fail("the failed ingest left residue on some node");
    }
    context.emit("no node kept a table or a system.datasets entry for the failed ingest");

    TestResult result = TestResult::pass(
        "a participant that never fetched failed the whole ingest, with a reason that "
        "says to re-run it, no dataset reported, and no partial table or catalog entry "
        "left on any node");
    result.metric("nodes", std::to_string(nodes))
        .metric("nodes doing work", std::to_string(nodes - 1))
        .metric("source chunks", std::to_string(chunks))
        .metric("coordination timeout", "8s")
        .metric("statement submissions", std::to_string(source.submits.load()));
    return result;
}

}  // namespace

void register_connect_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "cluster.connections",
        .category = TestCategory::Distributed,
        .name = "Saved connections: cluster-wide, read-only, no leak",
        .description =
            "The connector connection surface over a real cluster, against an "
            "injected fake transport (no network, no credentials, no warehouse). "
            "A saved connection round-trips through the replicate-to-all store and "
            "resolves with its credential on every node; a write statement (DROP / "
            "INSERT / a smuggled 'SELECT 1; DROP …') is refused before any request "
            "is built, with the transport never called; the token appears on no read "
            "surface (to_json, system.connections, the list_connections capability) "
            "on any node; and a query returns typed cells — number, boolean, null — "
            "rather than a table of strings (#827).",
        .options = {TestOption::integer("nodes", "Nodes", 3, 2, 6)},
        .run = run_connections,
    });
    suite.add(TestCase{
        .id = "cluster.distributed_import",
        .category = TestCategory::Distributed,
        .name = "Distributed connector import (co-launch)",
        .description =
            "The co-launched connector ingest over a real multi-node cluster with a "
            "fake warehouse: one statement is submitted for the whole cluster, every "
            "node pulls only its own tile straight from the source and lands it "
            "locally. Asserts every source chunk was fetched exactly once "
            "cluster-wide (attributed per node), that every partition is owned by the "
            "node that pulled it (land-where-you-pulled, RF=1, no re-placement), that "
            "the replicated document holds ZERO row entries — nothing crossed the "
            "wire twice — and that the dataset reads back complete and correct "
            "through the scatter-gather SELECT (#833).",
        .options = {TestOption::integer("nodes", "Nodes", 3, 2, 6),
                    TestOption::integer("chunks", "Source chunks", 9, 2, 256,
                                        "Independently-fetchable result partitions the "
                                        "source advertises; raised to nodes+1 if lower, "
                                        "since chunk 0 arrives inline."),
                    TestOption::integer("rows", "Rows per chunk", 6, 1, 2000)},
        .run = run_distributed_import,
    });
    suite.add(TestCase{
        .id = "cluster.import_participant_loss",
        .category = TestCategory::Distributed,
        .name = "Distributed import: a lost participant fails the whole ingest",
        .description =
            "All-or-nothing, demonstrated in-app rather than only at L1 (#840). One "
            "node of a real multi-node cluster stays in the roster but never drains "
            "the launch — the shape of a node that stopped doing work mid-ingest — so "
            "its tile is never fetched and it never reports in. Asserts the ingest "
            "FAILS rather than reporting a partial result set as a dataset, that the "
            "reason tells the caller to re-run it, and that no node keeps a table or "
            "a system.datasets entry for it. Nothing is wedged: the case owns its own "
            "loopback nodes and a node is made idle by not giving it a tick loop, so "
            "the panel leaves nothing behind.",
        .options = {TestOption::integer("nodes", "Nodes", 3, 2, 6)},
        .run = run_import_participant_loss,
    });
}

}  // namespace zetta::testkit
