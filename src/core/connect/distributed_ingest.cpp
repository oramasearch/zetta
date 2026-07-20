#include "zetta/connect/distributed_ingest.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/connect/distributed_fetch.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_manifest.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/fetch_affinity_placement.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::connect {
namespace {

using Json = nlohmann::json;
using LaunchOutcome = compute::CoordinatedLaunchListener::LaunchOutcome;

// Collects the first validation failure through the error-handler overload, so
// nothing crosses the validator-library boundary as an exception (the
// churn-payload / signal-envelope pattern).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const Json::json_pointer& pointer, const Json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

nlohmann::json_schema::json_validator& launch_validator() {
    static nlohmann::json_schema::json_validator validator(
        connector_fetch_launch_schema());
    return validator;
}

// What one node's tile came to.
struct TileOutcome {
    bool ok = false;
    std::string detail;
    std::size_t partitions_fetched = 0;  // source chunks pulled by THIS node
    std::size_t rows = 0;
    std::uint64_t bytes = 0;             // body bytes this node wrote
    std::vector<std::shared_ptr<data::DatasetPartition>> partitions;
};

// Undo the ingest on this node. A failed ingest leaves NOTHING behind: the
// bodies are unlinked (dropping their `lo:` keys) and the catalog entries
// dropped (their `part:` keys), so a node's view converges on "the dataset does
// not exist" rather than on a half-populated one.
//
// It discards **every** catalog entry for the table, not only the ones this node
// wrote. Two reasons: the whole dataset is being abandoned, so a peer's entry is
// residue either way; and a removal only reliably converges when the node
// holding a key removes it itself — a peer that merely merged the key keeps its
// copy, so every node clearing what it can see is what makes the catalog agree.
void discard_ingest(Node& node, data::Database& database,
                    const std::string& table_id) {
    for (const std::shared_ptr<data::DatasetPartition>& partition :
         node.dataset_partitions()) {  // a copy — the loop drops entries
        if (!partition || partition->table() != table_id) continue;
        if (!partition->blob_ref().empty()) {
            database.large_objects().unlink(partition->blob_ref());
        }
        node.drop_dataset_partition(partition->id());
    }
}

// Fetch `slice` from the source and land it as partitions THIS node owns.
// `carried` is anything the caller already holds (the initiator's inline
// partition 0). Non-throwing; a failure comes back in `detail`.
TileOutcome fetch_and_land_tile(Node& node, data::Database& database,
                                data::Table& table,
                                const ConnectorConfig& config,
                                const StatementPlan& plan,
                                const std::vector<std::size_t>& slice,
                                std::vector<Json> carried,
                                std::uint32_t partition_count,
                                std::size_t max_rows,
                                std::size_t replication_factor,
                                const std::vector<std::string>& members,
                                bool coordinated) {
    TileOutcome outcome;
    std::vector<Json> rows = std::move(carried);

    if (!slice.empty() && rows.size() < max_rows) {
        // Concurrent when the build has an async client, serial otherwise —
        // either way this node reads ONLY its own tile.
        const AsyncHttpTransport& concurrent = default_async_http_transport();
        const PartitionFetchResult fetched =
            concurrent
                ? fetch_result_partitions(config, plan, slice,
                                          max_rows - rows.size(),
                                          kDefaultPartitionsInFlight, concurrent)
                : fetch_result_partitions_serially(config, plan, slice,
                                                   max_rows - rows.size(),
                                                   default_http_transport());
        if (!fetched.ok()) {
            outcome.detail = fetched.error;
            return outcome;
        }
        outcome.partitions_fetched = fetched.partitions_fetched;
        for (const Json& row : fetched.rows) {
            if (rows.size() >= max_rows) break;
            rows.push_back(row);
        }
    }

    // Land it. The table is partition-only, so add_row writes the local staging
    // store and NOTHING is captured — the rows never re-replicate to the peer
    // that coordinated the ingest, which is the entire point of tiling the
    // fetch.
    const std::size_t before = table.row_count();
    for (Json& row : rows) table.add_row(std::move(row));
    outcome.rows = table.row_count() - before;

    // Fetch affinity: every partition this node writes is owned by this node.
    table.set_partitioner(std::make_shared<data::FetchAffinityPlacement>(
        partition_count, node.node_id(),
        std::min(std::max<std::size_t>(replication_factor, 1),
                 members.empty() ? std::size_t{1} : members.size())));
    // A tile records no manifest: it knows only its own share, and a manifest
    // describing a third of the dataset would fail every read of a healthy one
    // (#837). The coordinator writes it from the cluster-wide totals the
    // agreement below already produces.
    data::PartitionWriteResult written = data::write_partitioned(
        node, database, table, members, /*record_manifest=*/!coordinated);
    outcome.partitions = std::move(written.partitions);
    outcome.bytes = written.bytes_written;
    if (!written.ok()) {
        outcome.detail = "partition: " + written.error;
        return outcome;
    }
    outcome.ok = true;
    return outcome;
}

// The agreement. Every participant contributes {landed?, rows}; the ingest
// succeeds only when the `landed` column sums to the FULL roster size. That one
// comparison covers every failure mode at once: a node that died contributes
// nothing (the round times out, or the survivors' collective is short a
// contribution), a node whose fetch failed contributes 0, and a node that never
// received the launch contributes nothing. `rows` comes back as the cluster-wide
// total for free — and so, in the same round, do the cluster-wide **partition
// count and body size**, which is exactly the manifest the dataset needs (#837).
// The coordinator cannot derive that count any other way: only non-empty
// partitions get a catalog entry, so how many each tile produced is data.
struct Agreement {
    bool ok = false;
    std::size_t rows = 0;
    std::uint64_t partitions = 0;  // catalog entries written cluster-wide
    std::uint64_t bytes = 0;       // body bytes written cluster-wide
    std::string detail;
};

Agreement agree_on_the_ingest(compute::JobCoordinator& coordinator,
                              const std::string& launch_id,
                              std::size_t roster_size, bool landed,
                              std::size_t rows, std::uint64_t partitions,
                              std::uint64_t bytes,
                              std::chrono::milliseconds timeout) {
    Agreement agreement;
    const std::string tag = std::string(kConnectorFetchCollectiveTag) + launch_id;
    const std::optional<std::vector<double>> reduced =
        coordinator.collectives().allreduce(
            tag,
            {landed ? 1.0 : 0.0, static_cast<double>(rows),
             static_cast<double>(partitions), static_cast<double>(bytes)},
            zetta::cluster::CollectiveOp::Sum, timeout);
    if (!reduced || reduced->size() != 4) {
        agreement.detail =
            "the cluster did not agree on the ingest within " +
            std::to_string(timeout.count()) +
            "ms — a participant is unreachable or stopped mid-fetch; the "
            "dataset was discarded, re-run the ingest";
        return agreement;
    }
    const std::size_t landed_nodes = static_cast<std::size_t>((*reduced)[0]);
    if (landed_nodes != roster_size) {
        agreement.detail =
            "only " + std::to_string(landed_nodes) + " of " +
            std::to_string(roster_size) +
            " participants landed their tile — the dataset was discarded, "
            "re-run the ingest";
        return agreement;
    }
    agreement.ok = true;
    agreement.rows = static_cast<std::size_t>((*reduced)[1]);
    agreement.partitions = static_cast<std::uint64_t>((*reduced)[2]);
    agreement.bytes = static_cast<std::uint64_t>((*reduced)[3]);
    return agreement;
}

// A free table name in `database` starting from `base` (mirrors the one in
// add_dataset.cpp — loading the same query twice must not collide).
std::string unique_table_name(const data::Database& database, std::string base) {
    if (!database.find_table(base)) return base;
    for (int suffix = 2;; ++suffix) {
        std::string candidate = base + "_" + std::to_string(suffix);
        if (!database.find_table(candidate)) return candidate;
    }
}

std::vector<std::string> string_array(const Json& value) {
    std::vector<std::string> items;
    if (!value.is_array()) return items;
    for (const Json& entry : value) {
        if (entry.is_string()) items.push_back(entry.get<std::string>());
    }
    return items;
}

std::vector<std::uint64_t> row_count_array(const Json& value) {
    std::vector<std::uint64_t> counts;
    if (!value.is_array()) return counts;
    for (const Json& entry : value) {
        counts.push_back(entry.is_number_unsigned()
                             ? entry.get<std::uint64_t>()
                             : std::uint64_t{0});
    }
    return counts;
}

// The peer-side body of a `connector_fetch` launch, run on the node-owning
// thread by the listener's drain. It ALWAYS reaches the agreement (even when it
// has nothing to contribute but a zero), because a participant that walks away
// silently would leave the rest of the roster waiting out the whole timeout.
LaunchOutcome run_connector_fetch_launch(
    Node& node, const std::string& self,
    const compute::CoordinatedLaunchRequest& request) {
    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        return LaunchOutcome{.ok = false,
                             .detail = "no compute plane attached to this node"};
    }

    const std::optional<std::size_t> rank = compute::participant_rank(request, self);
    if (!rank) {
        // Stale membership: this node is not in the roster, so it owns no tile
        // and must NOT contribute — the roster's own members are the agreement.
        return LaunchOutcome{.ok = false,
                             .detail = "this node is not in the launch roster"};
    }

    // The payload boundary: schema-validate before reading a field.
    collecting_error_handler handler;
    launch_validator().validate(request.payload, handler);
    if (handler.failed) {
        agree_on_the_ingest(*coordinator, request.id,
                            request.participants.size(), /*landed=*/false, 0, 0,
                            0, kDefaultIngestCoordination);
        return LaunchOutcome{.ok = false,
                             .detail = "connector fetch payload: " + handler.reason};
    }

    const Json& payload = request.payload;
    const std::chrono::milliseconds timeout{
        payload.at("coordination_timeout_ms").get<std::int64_t>()};

    const auto decline = [&](std::string reason) {
        agree_on_the_ingest(*coordinator, request.id,
                            request.participants.size(), /*landed=*/false, 0, 0,
                            0, timeout);
        return LaunchOutcome{.ok = false, .detail = std::move(reason)};
    };

    // The credential is resolved HERE, from this node's own replicated copy of
    // the saved connection. Nothing about it travelled with the launch.
    const std::string connection = payload.at("connection").get<std::string>();
    const std::optional<SavedConnection> saved = find_connection(node, connection);
    if (!saved) {
        return decline("no saved connection named '" + connection +
                       "' on this node");
    }

    std::shared_ptr<data::Table> table = node.adopt_replicated_table(
        payload.at("database").get<std::string>(),
        payload.at("database_name").get<std::string>(),
        payload.at("table").get<std::string>(),
        payload.at("table_name").get<std::string>(), /*partition_only=*/true);
    if (!table) return decline("could not adopt the ingest's target table");
    std::shared_ptr<data::Database> database =
        node.find_database(payload.at("database_name").get<std::string>());
    if (!database) return decline("could not adopt the ingest's target database");

    StatementPlan plan;
    plan.handle = payload.at("handle").get<std::string>();
    plan.columns = string_array(payload.at("columns"));
    plan.column_types = string_array(payload.at("column_types"));
    plan.partition_rows = row_count_array(payload.at("partition_rows"));

    // The SAME pure tiling every other participant derives — no plan travelled.
    const std::vector<PartitionAssignment> tiling =
        plan_fetch_assignments(request.participants, plan.partition_rows);
    const std::vector<std::size_t> slice =
        *rank < tiling.size() ? tiling[*rank].partitions
                              : std::vector<std::size_t>{};

    std::shared_ptr<compute::ComputeJob> job =
        node.create_job(compute::JobKind::Ingest);
    if (job) job->set_status(compute::JobStatus::Running);

    const TileOutcome outcome = fetch_and_land_tile(
        node, *database, *table, saved->config, plan, slice, /*carried=*/{},
        static_cast<std::uint32_t>(payload.at("partition_count").get<std::uint32_t>()),
        payload.at("max_rows").get<std::size_t>(),
        payload.at("replication_factor").get<std::size_t>(),
        node.cluster_members(), /*coordinated=*/true);

    const Agreement agreement = agree_on_the_ingest(
        *coordinator, request.id, request.participants.size(), outcome.ok,
        outcome.rows, outcome.partitions.size(), outcome.bytes, timeout);
    if (!agreement.ok) {
        discard_ingest(node, *database, table->id());
        // Drop the table this node ADOPTED, not just its partitions: adopting
        // captured `tbl:<id>` into this node's own document, and the initiator's
        // removal of the same key is concurrent with that write rather than
        // causally after it — so a table only the initiator dropped can come
        // back. Both sides removing it converges on gone.
        database->drop_table(table->name());
        if (job) {
            job->set_status(compute::JobStatus::Failed,
                            outcome.ok ? agreement.detail : outcome.detail);
        }
        return LaunchOutcome{.ok = false,
                             .detail = outcome.ok ? agreement.detail
                                                  : outcome.detail};
    }
    if (job) {
        job->set_status(compute::JobStatus::Succeeded,
                        "fetched " + std::to_string(outcome.partitions_fetched) +
                            " result partition(s), landed " +
                            std::to_string(outcome.rows) + " row(s) in " +
                            std::to_string(outcome.partitions.size()) +
                            " owned partition(s)");
    }
    return LaunchOutcome{.ok = true,
                         .detail = "landed " + std::to_string(outcome.rows) +
                                   " row(s) from " +
                                   std::to_string(slice.size()) +
                                   " result partition(s)"};
}

}  // namespace

const nlohmann::json& connector_fetch_launch_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "$id": "https://oramasearch.github.io/zetta/schemas/connector_fetch_launch.schema.json",
      "title": "zetta.ConnectorFetchLaunch",
      "description": "The cross-node connector fetch run.launch payload (#833 section 7): the statement handle, the result set's shape and per-partition row counts (from which every participant derives the SAME tiling), and the target table the initiator already minted. It names a SAVED CONNECTION rather than carrying a credential — saved connections are replicate-to-all so each node presents the token itself.",
      "type": "object",
      "required": ["connection", "handle", "columns", "column_types",
                   "partition_rows", "database", "database_name", "table",
                   "table_name", "partition_count", "max_rows",
                   "replication_factor", "coordination_timeout_ms"],
      "additionalProperties": false,
      "properties": {
        "connection":     { "type": "string", "minLength": 1, "maxLength": 64,
                            "description": "the saved connection each node resolves locally" },
        "handle":         { "type": "string", "minLength": 1,
                            "description": "the source's statement handle the result partitions are read off" },
        "columns":        { "type": "array", "items": { "type": "string" } },
        "column_types":   { "type": "array", "items": { "type": "string" } },
        "partition_rows": { "type": "array", "items": { "type": "integer", "minimum": 0 },
                            "description": "rows per result partition, in partition order — the tiling's weights" },
        "database":       { "type": "string", "minLength": 1 },
        "database_name":  { "type": "string", "minLength": 1 },
        "table":          { "type": "string", "minLength": 1 },
        "table_name":     { "type": "string", "minLength": 1 },
        "partition_count":     { "type": "integer", "minimum": 1 },
        "max_rows":            { "type": "integer", "minimum": 1 },
        "replication_factor":  { "type": "integer", "minimum": 1 },
        "coordination_timeout_ms": { "type": "integer", "minimum": 1 }
      }
    })JSONSCHEMA");
    return schema;
}

void install_connector_fetch_launch_runner(
    compute::CoordinatedLaunchListener& listener, Node& node) {
    // The runner is stored in (and so cannot outlive) the listener it captures.
    listener.set_runner(
        std::string(kConnectorFetchLaunchKind),
        [&node, &listener](const compute::CoordinatedLaunchRequest& request) {
            return run_connector_fetch_launch(node, listener.node_id(), request);
        });
}

DistributedIngestResult run_distributed_connector_ingest(
    Node& node, const ConnectorConfig& config,
    const DistributedIngestOptions& options) {
    DistributedIngestResult result;

    if (const std::string rejection = read_only_query_rejection(options.query);
        !rejection.empty()) {
        result.error = rejection;
        return result;
    }
    if (options.max_rows == 0) {
        result.error = "max_rows must be at least 1";
        return result;
    }
    const HttpTransport& exchange = default_http_transport();
    if (!exchange) {
        result.error =
            "no outbound HTTP transport is installed in this build — a "
            "connector query cannot reach an external source";
        return result;
    }

    // 1) SUBMIT — once, from here. The result partitions' row counts are the
    // tiling's weights; partition 0's rows come back inline with them.
    StatementSubmission submitted = submit_connector_statement(
        config, options.query, options.max_rows, exchange);
    if (!submitted.ok()) {
        result.error = submitted.error;
        return result;
    }
    result.columns = submitted.plan.columns;

    compute::JobCoordinator* coordinator = node.job_coordinator();
    compute::CoordinatedLaunchListener* listener = node.coordinated_launch();
    const std::vector<std::string> roster =
        coordinator ? coordinator->collectives().participants()
                    : std::vector<std::string>{};
    // Degrade to the local path when there is nobody to co-launch with, when the
    // whole result already arrived inline, or when the caller passed an inline
    // connector config (no saved connection means peers hold no credential).
    const bool coordinate = roster.size() > 1 && coordinator != nullptr &&
                            listener != nullptr && !options.connection.empty() &&
                            submitted.plan.fetchable_partitions() > 0;

    std::vector<std::size_t> rest;
    for (std::size_t index = 1; index < submitted.plan.partition_rows.size();
         ++index) {
        rest.push_back(index);
    }

    const std::vector<std::string> members = node.cluster_members();
    std::shared_ptr<data::Database> database = node.find_database(options.database);
    if (!database) database = node.create_database(options.database);
    if (!database) {
        result.error = "could not create database '" + options.database + "'";
        return result;
    }
    result.database = database;

    const std::string base =
        options.table_name.empty() ? "dataset" : options.table_name;
    // Partition-only from birth: the marker is fixed at create time and rides in
    // the `tbl:` value, so every participant reads the table the same way before
    // a single row is written.
    std::shared_ptr<data::Table> table = database->create_table(
        unique_table_name(*database, base), std::nullopt, /*partition_only=*/true);
    if (!table) {
        result.error = "could not create the target table";
        return result;
    }
    result.table = table;

    std::shared_ptr<compute::ComputeJob> job =
        node.create_job(compute::JobKind::Ingest);
    if (job) {
        result.job = job->id();
        job->set_status(compute::JobStatus::Running);
    }

    const auto fail = [&](std::string reason) {
        discard_ingest(node, *database, table->id());
        database->drop_table(table->name());
        result.table.reset();
        result.partitions.clear();
        result.error = std::move(reason);
        if (job) job->set_status(compute::JobStatus::Failed, result.error);
        return result;
    };

    if (!coordinate) {
        // The local path: this node pulls the whole result and lands it as
        // partitions it owns. Same landing shape as a tile, so a one-node
        // cluster and an N-node cluster produce the same dataset.
        const TileOutcome outcome = fetch_and_land_tile(
            node, *database, *table, config, submitted.plan, rest,
            std::move(submitted.inline_rows.rows), options.partition_count,
            options.max_rows, options.replication_factor, members,
            /*coordinated=*/false);
        if (!outcome.ok) return fail(outcome.detail);
        result.partitions = outcome.partitions;
        result.partitions_fetched = outcome.partitions_fetched;
        result.rows = outcome.rows;
        result.participants = roster.empty() ? members : roster;
        if (job) {
            job->set_status(compute::JobStatus::Succeeded,
                            "landed " + std::to_string(outcome.rows) +
                                " row(s) on this node");
        }
        return result;
    }

    // 2) LAUNCH — the roster and the row counts, nothing else. No credential, no
    // assignment: every participant derives its own tile from these inputs.
    const std::string launch_id = zetta::util::new_uuid();
    compute::CoordinatedLaunchRequest request;
    request.id = launch_id;
    request.kind = std::string(kConnectorFetchLaunchKind);
    request.initiator = listener->node_id();
    request.participants = roster;
    request.payload =
        Json{{"connection", options.connection},
             {"handle", submitted.plan.handle},
             {"columns", submitted.plan.columns},
             {"column_types", submitted.plan.column_types},
             {"partition_rows", submitted.plan.partition_rows},
             {"database", database->id()},
             {"database_name", database->name()},
             {"table", table->id()},
             {"table_name", table->name()},
             {"partition_count", options.partition_count},
             {"max_rows", options.max_rows},
             {"replication_factor", options.replication_factor},
             {"coordination_timeout_ms", options.coordination_timeout.count()}};
    listener->fan_out(request);
    result.coordinated = true;
    result.participants = roster;

    // 3+4) FETCH + LAND — this node's own tile, plus the inline partition 0 it
    // already holds (which is why plan_fetch_assignments never tiles index 0).
    const std::size_t rank =
        compute::participant_rank(request, listener->node_id()).value_or(0);
    const std::vector<PartitionAssignment> tiling =
        plan_fetch_assignments(roster, submitted.plan.partition_rows);
    const std::vector<std::size_t> slice =
        rank < tiling.size() ? tiling[rank].partitions : std::vector<std::size_t>{};

    const TileOutcome outcome = fetch_and_land_tile(
        node, *database, *table, config, submitted.plan, slice,
        std::move(submitted.inline_rows.rows), options.partition_count,
        options.max_rows, options.replication_factor, members,
        /*coordinated=*/true);

    // All or nothing, cluster-wide.
    const Agreement agreement =
        agree_on_the_ingest(*coordinator, launch_id, roster.size(), outcome.ok,
                            outcome.rows, outcome.partitions.size(),
                            outcome.bytes, options.coordination_timeout);
    if (!agreement.ok) {
        return fail(outcome.ok ? agreement.detail : outcome.detail);
    }

    // The dataset is complete, and the allreduce just said how big it is. Record
    // the manifest ONCE, here, on the initiator (#837): it is the authority every
    // node — including this one — checks its view of the `part:` catalog against,
    // so that a partition still in flight fails the query instead of quietly
    // dropping out of the plan. Written under `mfst:<table-id>`, whose only
    // writer is this call.
    data::DatasetManifestValue manifest;
    manifest.table = table->id();
    manifest.partitions = agreement.partitions;
    manifest.rows = agreement.rows;
    manifest.bytes = agreement.bytes;
    manifest.created = zetta::util::now_iso8601();
    table->set_partition_manifest(std::move(manifest));

    result.partitions = outcome.partitions;
    result.partitions_fetched = outcome.partitions_fetched;
    result.rows = agreement.rows;  // the cluster-wide total
    if (job) {
        job->set_status(compute::JobStatus::Succeeded,
                        "co-fetched with " + std::to_string(roster.size()) +
                            " node(s): " + std::to_string(agreement.rows) +
                            " row(s) landed cluster-wide, " +
                            std::to_string(outcome.rows) + " on this node");
    }
    return result;
}

}  // namespace zetta::connect
