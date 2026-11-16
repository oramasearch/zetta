// SPDX-License-Identifier: MIT

// #833 section 7 — the **cross-node connector ingest**. Not "the tiling is
// correct" (tests/connect/test_distributed_fetch.cpp already pins that on the
// algorithm) but the properties of the thing actually running over a real
// loopback cluster:
//
//   1. **Exactly-once cluster-wide** — the union of what the nodes fetched is
//      the whole result set; no source chunk read twice, none missed. The fake
//      warehouse records which THREAD (and so which node) read each chunk.
//   2. **Land where you pulled** — every partition a node wrote is owned by
//      that node, and its rows are the chunks that node fetched.
//   3. **Rows never cross the wire twice** — the table is partition-only, so
//      the replicated document carries no `row:` entry for it at all.
//   4. **The dataset is readable and correct** afterwards, through the
//      scatter-gather SELECT, matching a single-node reference over the same
//      input.
//   5. **A node that stops mid-fetch fails the ingest**, with a clear reason and
//      no dataset left behind on any node.
//
// The transport is a fake installed as the process-wide default (that is how a
// PEER reaches the source — it never sees the initiator's injected argument),
// synchronous only: a zetta-core test has no async client, so every node takes
// the documented serial degrade. The concurrent, several-in-flight read within
// one node is covered by test_distributed_fetch.cpp.
//
// Timing-bound like every multi-node suite: convergence polls against a
// seconds-long deadline, never a fixed small count (tests/CLAUDE.md).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/connect/distributed_ingest.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/partition_rebalance.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::connect::ConnectorConfig;
using zetta::connect::DistributedIngestOptions;
using zetta::connect::DistributedIngestResult;
using zetta::connect::HttpRequest;
using zetta::connect::HttpResponse;
using zetta::query::QueryService;
using zetta::test::TempDir;
using Json = nlohmann::json;

constexpr std::size_t kChunks = 9;      // result partitions the source returns
constexpr std::size_t kRowsPerChunk = 6;
constexpr std::size_t kTotalRows = kChunks * kRowsPerChunk;

ConnectorConfig warehouse_config() {
    ConnectorConfig config;
    config.type = "snowflake";
    config.account = "ab12345.us-east-1";
    config.token = "a-token";
    config.warehouse = "COMPUTE_WH";
    return config;
}

// A fake warehouse whose result set is `kChunks` independently-fetchable
// partitions. It records, per calling THREAD, which chunks that thread read —
// which is what makes "who fetched what" observable across the cluster: the
// initiator fetches on the test thread and each peer on its own tick thread.
struct FakeWarehouse {
    std::mutex mutex;
    std::map<std::size_t, int> reads;                          // chunk -> times read
    std::map<std::thread::id, std::set<std::size_t>> by_thread;
    std::set<std::size_t> failing;  // chunks whose read reports a dead link
    std::atomic<int> submits{0};

    // Row `ordinal` of chunk `chunk`: a globally unique id, the chunk it came
    // from, and a value to aggregate.
    static Json wire_rows(std::size_t chunk) {
        Json data = Json::array();
        for (std::size_t index = 0; index < kRowsPerChunk; ++index) {
            const std::size_t ordinal = chunk * kRowsPerChunk + index;
            data.push_back(Json::array({std::to_string(ordinal),
                                        std::to_string(chunk),
                                        std::to_string(ordinal * 3)}));
        }
        return data;
    }

    static Json metadata() {
        Json partitions = Json::array();
        for (std::size_t index = 0; index < kChunks; ++index) {
            partitions.push_back({{"rowCount", kRowsPerChunk}});
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
            return response;
        }
        const std::size_t marker = request.url.find("partition=");
        const std::size_t chunk =
            marker == std::string::npos
                ? 0
                : static_cast<std::size_t>(
                      std::stoul(request.url.substr(marker + 10)));
        {
            const std::lock_guard<std::mutex> lock(mutex);
            ++reads[chunk];
            by_thread[std::this_thread::get_id()].insert(chunk);
        }
        if (failing.contains(chunk)) {
            response.status = 0;
            response.error = "connection reset by peer";
            return response;
        }
        response.body = Json{{"data", wire_rows(chunk)}}.dump();
        return response;
    }

    std::set<std::size_t> chunks_read_by(std::thread::id thread) {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto found = by_thread.find(thread);
        return found == by_thread.end() ? std::set<std::size_t>{} : found->second;
    }
};

// Installs the fake as the process-wide default transport for the life of the
// scope, and uninstalls it afterwards so no other suite can reach it.
class InstalledTransport {
public:
    explicit InstalledTransport(FakeWarehouse& source) {
        zetta::connect::set_default_http_transport(
            [&source](const HttpRequest& request) { return source.answer(request); },
            {});  // no async client in a zetta-core test: the serial degrade
    }
    ~InstalledTransport() { zetta::connect::set_default_http_transport({}, {}); }
    InstalledTransport(const InstalledTransport&) = delete;
    InstalledTransport& operator=(const InstalledTransport&) = delete;
};

// One live cluster member, wired the way main() wires a real process (the churn
// coordinated-launch harness): the replication actor id doubles as the cluster
// node id, bodies ride the blob store, and the node owns its compute plane —
// which is where the `run.launch` listener (and so the ingest runner) lives.
struct Member {
    Member(std::uint16_t port, const std::vector<std::string>& seeds)
        : replication(id),
          node(node_config(dir), &replication),
          blobs(dir.path(), replication),
          cluster(id, cluster_config(port, seeds), &replication, &blobs) {
        node.start();
        cluster.start();
        replication.set(
            "node:" + id,
            Json{{"address", cluster.listen_addresses().front()}});
        node.attach_cluster(cluster, /*worker_count=*/2);
    }
    ~Member() { node.stop(); }

    static NodeConfig node_config(const TempDir& dir) {
        NodeConfig config;
        config.data_dir = dir.path().string();
        config.preflight = false;
        // Every member shares the root secret, as main() passes each the cluster
        // join token: a credential one member sealed, every member opens (#1230).
        config.connection_root_secret = "distributed-ingest-test-cluster-secret";
        return config;
    }
    static ClusterConfig cluster_config(std::uint16_t port,
                                        const std::vector<std::string>& seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 4;
        config.seeds = seeds;
        return config;
    }

    TempDir dir;  // declared first: outlives everything below
    std::string id = zetta::util::new_uuid();
    ReplicationManager replication;
    Node node;
    zetta::data::ReplicatedBlobStore blobs;
    Cluster cluster;
};

std::vector<std::unique_ptr<Member>> bring_up(std::uint16_t base_port,
                                              std::size_t count) {
    std::vector<std::unique_ptr<Member>> members;
    std::vector<std::string> seeds;
    for (std::size_t index = 0; index < count; ++index) {
        members.push_back(std::make_unique<Member>(
            static_cast<std::uint16_t>(base_port + index * 4), seeds));
        seeds.push_back(members.back()->cluster.listen_address());
    }
    return members;
}

// How many `part:` entries for `table_id` this node's replicated document holds
// versus how many its node tree has materialized. The two must converge: a
// document entry the projection never materializes is invisible to the planner,
// which is the shape #837 turned out to be (a tick recorded a version it had not
// reconciled, and with no further write to move the version it never revisited
// it). Reported on a convergence failure so the next one is diagnosable rather
// than a bare timeout.
std::string catalog_state(Member& member, const std::string& table_id) {
    std::size_t in_document = 0;
    for (const auto& [key, value] : member.replication.snapshot().items()) {
        if (!key.starts_with("part:")) continue;
        if (value.value("table", std::string{}) == table_id) ++in_document;
    }
    std::size_t in_tree = 0;
    for (const auto& partition : member.node.dataset_partitions()) {
        if (partition && partition->table() == table_id) ++in_tree;
    }
    return "document=" + std::to_string(in_document) +
           " tree=" + std::to_string(in_tree);
}

// Patient: poll `ready` against a seconds-long deadline (never a fixed count).
template <class Predicate>
bool wait_until(Predicate ready,
                std::chrono::seconds budget = std::chrono::seconds(30)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return ready();
}

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

DistributedIngestOptions ingest_options() {
    DistributedIngestOptions options;
    options.connection = "warehouse";
    options.query = "SELECT id, chunk, amount FROM source";
    options.table_name = "cached";
    options.partition_count = 4;
    options.replication_factor = 1;  // a result set is a cache
    options.coordination_timeout = std::chrono::seconds(20);
    return options;
}

// The rows a node holds locally — its staging copy of exactly the tile it
// fetched, which is what write_partitioned serialized into the bodies it owns.
std::set<std::size_t> local_chunks(Node& node, const std::string& table_name,
                                   std::size_t* rows = nullptr) {
    std::set<std::size_t> chunks;
    const auto database = node.find_database("datasets");
    if (!database) return chunks;
    const auto table = database->find_table(table_name);
    if (!table) return chunks;
    const zetta::storage::StorageEngine& store = table->store();
    if (rows != nullptr) *rows = store.size();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const zetta::storage::ParquetLazyPtr row = store.at(index);
        if (!row || !row->is_object()) continue;
        chunks.insert(row->at("chunk").get<std::size_t>());
    }
    return chunks;
}

// Every `row:` key in the replicated document that belongs to `table_id`.
std::size_t replicated_row_entries(ReplicationManager& replication,
                                   const std::string& table_id) {
    std::size_t rows = 0;
    const Json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (!key.starts_with("row:") || !value.is_object()) continue;
        if (value.value("tbl", std::string{}) == table_id) ++rows;
    }
    return rows;
}

}  // namespace

// ---------------------------------------------------------------------------
// The single-node reference: the same query, the same source, one node. The
// coordinated run below must land the identical dataset.
// ---------------------------------------------------------------------------
TEST(DistributedIngest, SingleNodeClusterTakesTheLocalPathAndLandsEverything) {
    FakeWarehouse source;
    InstalledTransport installed(source);

    const auto members = bring_up(24810, 1);
    ASSERT_TRUE(zetta::connect::save_connection(members[0]->node, "warehouse",
                                                warehouse_config())
                    .empty());

    const DistributedIngestResult result =
        run_distributed_connector_ingest(members[0]->node, warehouse_config(),
                                         ingest_options());
    ASSERT_TRUE(result.ok()) << result.error;
    // Nobody to co-launch with: the documented degrade, not a co-launch of one.
    EXPECT_FALSE(result.coordinated);
    EXPECT_EQ(result.rows, kTotalRows);
    EXPECT_EQ(result.partitions_fetched, kChunks - 1);  // chunk 0 came inline
    EXPECT_EQ(source.reads[0], 0) << "chunk 0 arrived inline; it must not be re-read";
    ASSERT_TRUE(result.table);

    // Readable through the scatter-gather SELECT, and correct.
    QueryService service{members[0]->node};
    const auto all =
        service.run("SELECT * FROM datasets." + result.table->name() + ";", "");
    ASSERT_TRUE(all.ok()) << all.error;
    EXPECT_EQ(all.rows.size(), kTotalRows);
}

// A connector that is not Snowflake must never be run through Snowflake's
// submit/tile protocol. It was: `create_dataset_from_query` against a Postgres
// connection built a Snowflake REST request out of the config's empty
// `account` and failed with "could not resolve .snowflakecomputing.com" — a
// vendor the caller never named, for a source that was on 127.0.0.1. The bug
// hid behind the consent gate for as long as the gate refused the call first
// (#994, #995). Pin the dispatch here, with no Postgres required: the reason
// that comes back must be about the Postgres connector, never about Snowflake.
TEST(DistributedIngest, ANonSnowflakeConfigNeverTakesTheSnowflakeProtocol) {
    FakeWarehouse source;      // installed, and deliberately never reached
    InstalledTransport installed(source);

    const auto members = bring_up(24870, 1);

    ConnectorConfig postgres;
    postgres.type = "postgres";
    postgres.host = "127.0.0.1";
    postgres.port = "5433";
    postgres.user = "analyst";
    postgres.database = "analytics";

    DistributedIngestOptions options = ingest_options();
    options.connection.clear();  // inline config; no saved connection

    const DistributedIngestResult result =
        run_distributed_connector_ingest(members[0]->node, postgres, options);

    // An isolated core test has no Postgres runner; an earlier API journey can
    // install one for the process. Both failures must name the right connector,
    // regardless of capitalization. Snowflake here means dispatch is wrong.
    EXPECT_EQ(result.error.find("snowflake"), std::string::npos) << result.error;
    EXPECT_EQ(result.error.find("Snowflake"), std::string::npos) << result.error;
    EXPECT_TRUE(result.error.find("Postgres") != std::string::npos ||
                result.error.find("postgres") != std::string::npos) << result.error;
    EXPECT_EQ(source.submits, 0)
        << "the Snowflake protocol was entered for a postgres config";
}

// ---------------------------------------------------------------------------
// The acceptance: three real members, one query, every node pulling its own
// tile straight from the source and landing it locally.
// ---------------------------------------------------------------------------
TEST(DistributedIngest, CoLaunchFetchesEachChunkOnceAndLandsWhereItPulled) {
    FakeWarehouse source;
    InstalledTransport installed(source);

    const auto members = bring_up(24820, 3);
    ASSERT_TRUE(wait_until([&] {
        for (const auto& member : members) {
            if (member->cluster.peer_count() != members.size() - 1) return false;
        }
        return true;
    })) << "3-node loopback cluster did not converge";

    // The credential is saved ONCE and replicates to every node — that is what
    // lets a peer present it itself, so nothing secret rides the launch.
    ASSERT_TRUE(zetta::connect::save_connection(members[0]->node, "warehouse",
                                                warehouse_config())
                    .empty());
    ASSERT_TRUE(wait_until([&] {
        for (const auto& member : members) {
            member->node.tick();
            if (!zetta::connect::find_connection(member->node, "warehouse")) {
                return false;
            }
        }
        return true;
    })) << "the saved connection did not reach every node";

    // The peers run their own node loops; the initiator submits from this
    // thread (so the initiator's tile is attributable to this thread).
    TickLoop peer_one(members[1]->node);
    TickLoop peer_two(members[2]->node);

    const DistributedIngestResult result =
        run_distributed_connector_ingest(members[0]->node, warehouse_config(),
                                         ingest_options());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_TRUE(result.coordinated);
    EXPECT_EQ(result.participants.size(), 3u);
    ASSERT_TRUE(result.table);
    const std::string table_name = result.table->name();
    const std::string table_id = result.table->id();

    // The statement was submitted ONCE for the whole cluster.
    EXPECT_EQ(source.submits.load(), 1);

    // --- Property 1: exactly-once cluster-wide --------------------------------
    // Every chunk past 0 read exactly once, chunk 0 never re-read (it arrived
    // inline with the submission), and nothing missed.
    EXPECT_EQ(source.reads[0], 0) << "chunk 0 was fetched a second time";
    for (std::size_t chunk = 1; chunk < kChunks; ++chunk) {
        EXPECT_EQ(source.reads[chunk], 1)
            << "chunk " << chunk << " was read " << source.reads[chunk] << " times";
    }
    // …and the reads are disjoint across the three fetching threads, i.e. each
    // NODE pulled its own tile rather than one node pulling everything.
    const std::vector<std::set<std::size_t>> pulled = {
        source.chunks_read_by(std::this_thread::get_id()),
        source.chunks_read_by(peer_one.thread_id()),
        source.chunks_read_by(peer_two.thread_id())};
    std::multiset<std::size_t> union_of_tiles;
    for (const std::set<std::size_t>& tile : pulled) {
        for (const std::size_t chunk : tile) union_of_tiles.insert(chunk);
    }
    EXPECT_EQ(union_of_tiles.size(), kChunks - 1) << "the tiles overlap";
    for (std::size_t chunk = 1; chunk < kChunks; ++chunk) {
        EXPECT_EQ(union_of_tiles.count(chunk), 1u) << "chunk " << chunk;
    }
    std::size_t fetching_nodes = 0;
    for (const std::set<std::size_t>& tile : pulled) {
        if (!tile.empty()) ++fetching_nodes;
    }
    EXPECT_GE(fetching_nodes, 2u)
        << "only one node fetched — that is a parallel loop, not a distributed "
           "ingest";

    // --- Property 4: the whole dataset is readable and correct ----------------
    // Patient, and with the peers still running their own loops: the catalog
    // entries each participant captured reach the coordinator on the next gossip
    // round, and the responders refresh their owned-partition snapshots from
    // their own ticks.
    QueryService service{members[0]->node};
    QueryService::Outcome all;
    ASSERT_TRUE(wait_until(
        [&] {
            members[0]->node.tick();  // the peers tick on their own loops
            all = service.run("SELECT * FROM datasets." + table_name + ";", "");
            return all.ok() && all.rows.size() == kTotalRows;
        },
        std::chrono::seconds(60)))
        << "the scattered read never saw the whole dataset: " << all.error
        << "\n  initiator: " << catalog_state(*members[0], table_id);

    std::set<std::int64_t> ids;
    std::int64_t total_amount = 0;
    for (const Json& row : all.rows) {
        ids.insert(row.at("id").get<std::int64_t>());
        total_amount += row.at("amount").get<std::int64_t>();
    }
    EXPECT_EQ(ids.size(), kTotalRows) << "a row came back twice";
    // The single-node reference over the same input: ids 0..53, amount = id*3.
    std::int64_t expected_amount = 0;
    for (std::size_t ordinal = 0; ordinal < kTotalRows; ++ordinal) {
        expected_amount += static_cast<std::int64_t>(ordinal) * 3;
    }
    EXPECT_EQ(total_amount, expected_amount);

    // A MERGED aggregate over the same scatter-gather, run **from a peer** —
    // a node that fetched only a third of the result and holds none of the
    // initiator's inline chunk. count/sum must add across the owners, not be
    // answered from that node's own slice.
    //
    // This used to run on the initiator to dodge #837: a partition whose `part:`
    // entry had not gossiped to the peer yet was *invisible*, so it dropped out
    // of the plan and the aggregate came back a plausible fraction with no
    // error. With the partition manifest in place a short catalog FAILS the
    // query instead, so waiting for `ok()` is a real convergence condition and
    // not a race against a silent undercount.
    //
    // peer_one's loop stops first so this thread owns members[1]'s tree; the
    // other two nodes keep ticking (members[0] here, members[2] on its own
    // loop), which is what refreshes their responder snapshots.
    peer_one.stop();
    QueryService peer_service{members[1]->node};
    const std::string aggregate_sql =
        "SELECT chunk, count(*), sum(amount) FROM datasets." + table_name +
        " GROUP BY chunk;";
    QueryService::Outcome aggregated;
    ASSERT_TRUE(wait_until(
        [&] {
            members[0]->node.tick();
            members[1]->node.tick();
            aggregated = peer_service.run(aggregate_sql, "");
            return aggregated.ok() && aggregated.rows.size() == kChunks;
        },
        std::chrono::seconds(60)))
        << "the peer never saw the whole dataset: " << aggregated.error
        << "\n  peer: " << catalog_state(*members[1], table_id);

    // Converged: now it must answer the SAME totals every time, not once.
    for (int attempt = 0; attempt < 5; ++attempt) {
        aggregated = peer_service.run(aggregate_sql, "");
        ASSERT_TRUE(aggregated.ok()) << "attempt " << attempt << ": "
                                     << aggregated.error;
        ASSERT_EQ(aggregated.rows.size(), kChunks) << "attempt " << attempt;
        std::int64_t merged_amount = 0;
        std::uint64_t merged_rows = 0;
        for (const Json& row : aggregated.rows) {
            merged_rows += row.at("count").get<std::uint64_t>();
            merged_amount +=
                static_cast<std::int64_t>(row.at("sum_amount").get<double>());
            EXPECT_EQ(row.at("count").get<std::uint64_t>(), kRowsPerChunk)
                << "attempt " << attempt << ", chunk " << row.at("chunk");
        }
        EXPECT_EQ(merged_rows, kTotalRows) << "attempt " << attempt;
        EXPECT_EQ(merged_amount, expected_amount) << "attempt " << attempt;
    }

    // The last peer loop stops here: from now on this thread is the only one
    // ticking, so their node trees (owner-thread-only) are safe to read.
    peer_two.stop();

    // --- Property 2: land where you pulled ------------------------------------
    // Each node holds exactly the rows of the chunks IT fetched (the initiator
    // additionally holds chunk 0, which came inline with its submission).
    for (std::size_t index = 0; index < members.size(); ++index) {
        std::set<std::size_t> expected = pulled[index];
        if (index == 0) expected.insert(0);
        std::size_t rows = 0;
        const std::set<std::size_t> held =
            local_chunks(members[index]->node, table_name, &rows);
        EXPECT_EQ(held, expected) << "node " << index << " holds the wrong chunks";
        EXPECT_EQ(rows, expected.size() * kRowsPerChunk) << "node " << index;
    }

    // The catalog agrees, and it says so on each node WITHOUT waiting for
    // gossip: a node's own `part:` entries are captured locally the moment it
    // writes them. Every entry a node produced names that node as its single
    // owner — not moved, not re-placed — and their row ranges account for
    // exactly the rows that node landed.
    std::uint64_t catalogued = 0;
    for (std::size_t index = 0; index < members.size(); ++index) {
        std::size_t rows = 0;
        local_chunks(members[index]->node, table_name, &rows);
        std::uint64_t owned = 0;
        for (const auto& partition :
             members[index]->node.dataset_partitions()) {
            if (partition->table() != table_id) continue;
            ASSERT_EQ(partition->owner_nodes().size(), 1u)
                << "a cached result set is RF=1";
            // Every owner is a participant — a hash-based placement would have
            // handed partitions to nodes that fetched nothing.
            EXPECT_NE(std::find(result.participants.begin(),
                                result.participants.end(),
                                partition->owner_nodes().front()),
                      result.participants.end())
                << "partition owned by a node that never fetched";
            if (partition->owner_nodes().front() != members[index]->id) continue;
            owned += partition->row_range().end - partition->row_range().start;
        }
        EXPECT_EQ(owned, rows) << "node " << index
                               << " owns a different number of rows than it landed";
        EXPECT_GT(owned, 0u) << "node " << index << " owns nothing";
        catalogued += owned;
    }
    // Together the participants own the whole result set, once.
    EXPECT_EQ(catalogued, kTotalRows);
    // …which is also what the cluster agreed on through the allreduce.
    EXPECT_EQ(result.rows, kTotalRows);

    // --- The fetch landed in the TARGET shape, with NO rebalance --------------
    // A Snowflake result set is a FLEXIBLE source: its chunks are freely
    // assignable, so the fetch was tiled by the plan's per-node shares and every
    // node wrote exactly the partitions the plan gave it. Nothing has to move
    // afterwards, and nothing did.
    ASSERT_TRUE(result.plan.ok()) << "the ingest ran without a plan";
    EXPECT_EQ(result.plan.owners.size(), result.plan.partition_count);
    std::size_t written_cluster_wide = 0;
    for (std::size_t index = 0; index < members.size(); ++index) {
        std::size_t written = 0;
        for (const auto& partition : members[index]->node.dataset_partitions()) {
            if (partition->table() != table_id) continue;
            if (partition->owner_nodes().front() != members[index]->id) continue;
            ++written;
        }
        written_cluster_wide += written;
        // Never MORE than planned. Fewer is legal and expected at this scale:
        // only non-empty partitions get a catalog entry, and 54 rows over a
        // plan's worth of partitions leaves some empty.
        EXPECT_LE(written, result.plan.primary_partitions_of(members[index]->id))
            << "node " << index << " wrote more partitions than the plan gave it: "
            << result.plan.rationale;
    }
    EXPECT_GT(written_cluster_wide, 0u);

    // **No partition movement.** Every catalog entry still names exactly the node
    // that wrote it (asserted above), and the placement recorded on each entry is
    // still fetch affinity — a rebalance would have rewritten it to rendezvous.
    for (std::size_t index = 0; index < members.size(); ++index) {
        for (const auto& partition : members[index]->node.dataset_partitions()) {
            if (partition->table() != table_id) continue;
            EXPECT_EQ(partition->stats().value("placement", std::string{}),
                      "fetch_affinity")
                << "a partition was re-placed — a flexible source must need no "
                   "rebalance";
        }
    }
    // …and running the opportunistic rebalance over the live membership moves
    // nothing, which is the property in its own words.
    const zetta::data::RebalanceResult idle = zetta::data::rebalance_partitions(
        members[0]->node, members[0]->node.cluster_members(),
        members[0]->node.cluster_members());
    EXPECT_EQ(idle.moved, 0u) << "the ingest's placement was disturbed";

    // --- Property 3: the rows never crossed the wire twice --------------------
    // The table is partition-only, so the replicated document holds NO row
    // entry for it — nothing re-replicated to the coordinator or anywhere else.
    for (std::size_t index = 0; index < members.size(); ++index) {
        EXPECT_EQ(replicated_row_entries(members[index]->replication, table_id), 0u)
            << "node " << index << " replicated rows of a partition-only table";
    }


    // Every participant recorded its share as a ComputeJob, so the ingest shows
    // up in system.jobs like other distributed work. Jobs are replicated, so
    // one per participant converges on every node.
    EXPECT_FALSE(result.job.empty());
    EXPECT_TRUE(members[0]->node.find_job(result.job));
    const auto ingest_jobs = [](Node& node) {
        std::size_t count = 0;
        for (const auto& job : node.jobs()) {
            if (job->kind() == zetta::compute::JobKind::Ingest &&
                job->status() == zetta::compute::JobStatus::Succeeded) {
                ++count;
            }
        }
        return count;
    };
    ASSERT_TRUE(wait_until([&] {
        for (const auto& member : members) member->node.tick();
        return ingest_jobs(members[0]->node) == members.size();
    })) << "not every participant recorded a succeeded ingest job";
}

// ---------------------------------------------------------------------------
// Property 5: a participant that stops mid-fetch fails the WHOLE ingest, and
// the dataset either exists complete or does not exist.
// ---------------------------------------------------------------------------
TEST(DistributedIngest, AParticipantThatStopsMidFetchFailsTheWholeIngest) {
    FakeWarehouse source;
    InstalledTransport installed(source);

    const auto members = bring_up(24840, 3);
    ASSERT_TRUE(wait_until([&] {
        for (const auto& member : members) {
            if (member->cluster.peer_count() != members.size() - 1) return false;
        }
        return true;
    })) << "3-node loopback cluster did not converge";

    ASSERT_TRUE(zetta::connect::save_connection(members[0]->node, "warehouse",
                                                warehouse_config())
                    .empty());
    ASSERT_TRUE(wait_until([&] {
        for (const auto& member : members) {
            member->node.tick();
            if (!zetta::connect::find_connection(member->node, "warehouse")) {
                return false;
            }
        }
        return true;
    })) << "the saved connection did not reach every node";

    // Only ONE peer runs a node loop. The third member is in the roster (it is
    // a connected, collective participant) but never drains the launch — a node
    // that stopped doing work mid-ingest. Its tile is never fetched and it never
    // reports in.
    TickLoop peer_one(members[1]->node);

    DistributedIngestOptions options = ingest_options();
    options.coordination_timeout = std::chrono::seconds(4);
    const DistributedIngestResult result =
        run_distributed_connector_ingest(members[0]->node, warehouse_config(),
                                         options);
    ASSERT_FALSE(result.ok()) << "a dead participant must fail the ingest";
    EXPECT_NE(result.error.find("re-run the ingest"), std::string::npos)
        << result.error;
    EXPECT_FALSE(result.table) << "a failed ingest must not report a dataset";

    // Stop the peer's loop first: joining its thread guarantees its own copy of
    // the launch has finished (and makes reading its node tree safe).
    peer_one.stop();

    // No partial dataset survives, on any node and on every surface a caller
    // reads: the table is gone and nothing by that name is listed in the
    // catalog-backed `system.datasets` view. Each participant discarded what it
    // had landed when the agreement failed.
    //
    // (What is NOT asserted, deliberately: that every `part:`/`lo:` key is gone
    // from the replicated document. A removal can be undone by a peer's gossip
    // merge — see docs/data/distributed-connector-ingest.md, "Residue" — which
    // is a property of the replication layer's delete-versus-merge behavior, not
    // of the ingest. The residue is invisible through every read surface because
    // the table it belongs to no longer exists.)
    for (const auto& member : members) {
        const auto database = member->node.find_database("datasets");
        EXPECT_FALSE(database && database->find_table("cached"))
            << "a failed ingest left its table behind";
        for (const auto& dataset : member->node.datasets()) {
            EXPECT_NE(dataset.name, "cached") << "a partial dataset survived";
        }
    }
}

// ---------------------------------------------------------------------------
// #844: a participant that was PAUSED, not dead, comes back after the ingest
// failed and was cleaned up. It still holds the `run.launch` it queued while
// paused, and draining it must not re-adopt the table every other node has
// dropped, fetch from the source for a run that has ended, or hold its owner
// thread waiting on an agreement nobody will join.
//
// "Died" and "came back" are different faults. The test above is the first;
// this is the second. The terminal marker the resuming node observes is the
// table's own tombstone: the cleanup removed `tbl:<id>`, and a removal
// replicates as a tombstone (#838), so the paused node's document already
// records that the table existed and was dropped.
// ---------------------------------------------------------------------------
TEST(DistributedIngest, APausedParticipantThatResumesDeclinesTheEndedLaunch) {
    FakeWarehouse source;
    InstalledTransport installed(source);

    const auto members = bring_up(24890, 3);
    ASSERT_TRUE(wait_until([&] {
        for (const auto& member : members) {
            if (member->cluster.peer_count() != members.size() - 1) return false;
        }
        return true;
    })) << "3-node loopback cluster did not converge";

    ASSERT_TRUE(zetta::connect::save_connection(members[0]->node, "warehouse",
                                                warehouse_config())
                    .empty());
    ASSERT_TRUE(wait_until([&] {
        for (const auto& member : members) {
            member->node.tick();
            if (!zetta::connect::find_connection(member->node, "warehouse")) {
                return false;
            }
        }
        return true;
    })) << "the saved connection did not reach every node";

    Member& paused = *members[2];
    TickLoop peer_one(members[1]->node);

    // The initiator mints the target table's id inside the ingest and reports
    // no table on failure, so read the id off the replicated document while the
    // ingest is waiting on its agreement.
    std::mutex table_mutex;
    std::string table_key;
    std::atomic<bool> ingest_done{false};
    std::thread watcher([&] {
        while (!ingest_done.load()) {
            const Json snapshot = members[0]->replication.snapshot();
            for (const auto& [key, value] : snapshot.items()) {
                if (key.starts_with("tbl:") && value.is_object() &&
                    value.value("name", std::string{}) == "cached") {
                    const std::lock_guard<std::mutex> lock(table_mutex);
                    table_key = key;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    DistributedIngestOptions options = ingest_options();
    options.coordination_timeout = std::chrono::seconds(4);
    const DistributedIngestResult result =
        run_distributed_connector_ingest(members[0]->node, warehouse_config(),
                                         options);
    ingest_done.store(true);
    watcher.join();
    peer_one.stop();

    ASSERT_FALSE(result.ok()) << "the paused participant never reported in, so "
                                 "the ingest must fail";
    std::string key;
    {
        const std::lock_guard<std::mutex> lock(table_mutex);
        key = table_key;
    }
    ASSERT_FALSE(key.empty()) << "never saw the ingest's target table";

    // The paused node is holding the launch it was sent: it has not ticked.
    ASSERT_EQ(paused.node.coordinated_launch()->pending(), 1u)
        << "the paused participant should still hold its queued launch";

    // Wait until the paused node's document has merged the cleanup — the table
    // is known to it, and known as removed. Only the running nodes tick: the
    // paused node merges gossip on its network thread, as a paused process's
    // socket still does, but executes nothing.
    ASSERT_TRUE(wait_until([&] {
        members[0]->node.tick();
        members[1]->node.tick();
        return paused.replication.knows_key(key) &&
               !paused.replication.snapshot().contains(key);
    })) << "the paused node never observed the failed ingest's cleanup";

    const auto total_reads = [&] {
        const std::lock_guard<std::mutex> lock(source.mutex);
        int reads = 0;
        for (const auto& [chunk, count] : source.reads) reads += count;
        return reads;
    };
    const int reads_before = total_reads();
    // Jobs replicate, so the resumed node's first tick also materializes the
    // running nodes' ingest jobs. Any job id outside theirs is one it started.
    std::set<std::string> running_jobs;
    for (std::size_t index = 0; index < 2; ++index) {
        for (const auto& job : members[index]->node.jobs()) {
            if (job) running_jobs.insert(job->id());
        }
    }

    // RESUME: one tick drains the queued launch on this, its owner thread.
    const auto resumed_at = std::chrono::steady_clock::now();
    paused.node.tick();
    const auto drain_took = std::chrono::steady_clock::now() - resumed_at;

    EXPECT_EQ(paused.node.coordinated_launch()->pending(), 0u)
        << "the resumed node did not drain its queued launch";
    EXPECT_EQ(total_reads(), reads_before)
        << "the resumed participant fetched from the source for an ingest that "
           "had already failed and been cleaned up";
    for (const auto& job : paused.node.jobs()) {
        EXPECT_TRUE(job && running_jobs.contains(job->id()))
            << "the resumed participant started an ingest job for an ended run";
    }
    EXPECT_LT(drain_took, options.coordination_timeout)
        << "the resumed participant waited on an agreement nobody will join";
    EXPECT_FALSE(paused.replication.snapshot().contains(key))
        << "the resumed participant re-adopted the dropped table";

    // And it stays gone everywhere, through every surface a caller reads.
    EXPECT_TRUE(wait_until(
        [&] {
            for (const auto& member : members) {
                member->node.tick();
                if (member->replication.snapshot().contains(key)) return false;
                const auto database = member->node.find_database("datasets");
                if (database && database->find_table("cached")) return false;
                for (const auto& dataset : member->node.datasets()) {
                    if (dataset.name == "cached") return false;
                }
            }
            return true;
        },
        std::chrono::seconds(10)))
        << "the failed ingest's table came back on some node";
}

// An inline connector config (rather than a saved connection) cannot be
// co-launched — the peers would hold no credential — so it degrades to this
// node, and still lands a complete, readable dataset.
TEST(DistributedIngest, AnInlineConnectorConfigDegradesToTheLocalPath) {
    FakeWarehouse source;
    InstalledTransport installed(source);

    const auto members = bring_up(24860, 2);
    ASSERT_TRUE(wait_until([&] {
        return members[0]->cluster.peer_count() == 1 &&
               members[1]->cluster.peer_count() == 1;
    }));

    TickLoop peer(members[1]->node);

    DistributedIngestOptions options = ingest_options();
    options.connection.clear();  // an inline config: no saved connection
    const DistributedIngestResult result =
        run_distributed_connector_ingest(members[0]->node, warehouse_config(),
                                         options);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_FALSE(result.coordinated);
    EXPECT_EQ(result.rows, kTotalRows);
    ASSERT_TRUE(result.table);

    QueryService service{members[0]->node};
    const auto all =
        service.run("SELECT * FROM datasets." + result.table->name() + ";", "");
    ASSERT_TRUE(all.ok()) << all.error;
    EXPECT_EQ(all.rows.size(), kTotalRows);
}

// A result that arrived wholly inline has nothing to fan out, so no launch is
// made even on a multi-node cluster.
TEST(DistributedIngest, ASingleResultPartitionIsNeverCoLaunched) {
    FakeWarehouse source;
    InstalledTransport installed(source);

    const auto members = bring_up(24880, 2);
    ASSERT_TRUE(wait_until([&] {
        return members[0]->cluster.peer_count() == 1 &&
               members[1]->cluster.peer_count() == 1;
    }));
    ASSERT_TRUE(zetta::connect::save_connection(members[0]->node, "warehouse",
                                                warehouse_config())
                    .empty());
    TickLoop peer(members[1]->node);

    // One result partition: it came inline with the submission.
    zetta::connect::set_default_http_transport(
        [](const HttpRequest& request) {
            HttpResponse response;
            response.status = 200;
            if (request.method != "POST") {
                response.status = 500;
                response.body = "{\"message\":\"no partition should be read\"}";
                return response;
            }
            Json data = Json::array();
            for (std::size_t index = 0; index < 4; ++index) {
                data.push_back(Json::array({std::to_string(index), "0",
                                            std::to_string(index * 3)}));
            }
            response.body =
                Json{{"statementHandle", "handle-1"},
                     {"resultSetMetaData",
                      Json{{"rowType",
                            Json::array({{{"name", "id"}, {"type", "fixed"}},
                                         {{"name", "chunk"}, {"type", "fixed"}},
                                         {{"name", "amount"}, {"type", "fixed"}}})},
                           {"partitionInfo", Json::array({{{"rowCount", 4}}})}}},
                     {"data", data}}
                    .dump();
            return response;
        },
        {});

    const DistributedIngestResult result =
        run_distributed_connector_ingest(members[0]->node, warehouse_config(),
                                         ingest_options());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_FALSE(result.coordinated);
    EXPECT_EQ(result.rows, 4u);
    EXPECT_EQ(result.partitions_fetched, 0u);
}
