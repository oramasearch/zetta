// SPDX-License-Identifier: MIT

#include "zetta/ddf/local_partitions.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

namespace zetta::ddf {
namespace {

// Read the body of a large object by id from whichever of the node's databases
// holds it. A `blob_ref` is a globally-unique `lo:` id, so the first database
// whose store has the body present locally is the right one; scanning is robust to
// the table living in a different database than expected. nullopt when no database
// holds the body locally.
std::optional<std::string> read_blob_body(Node& node, const std::string& blob_ref) {
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database = node.find_database(name);
        if (!database) continue;
        if (std::optional<std::string> body =
                database->large_objects().read(blob_ref)) {
            return body;
        }
    }
    return std::nullopt;
}

// Append a partition body's rows to `store`. The body is read through the shared
// partition-body reader, which sniffs Parquet vs the legacy JSON Lines from the
// bytes — and **fails fast** on a body it cannot read, since a silently short
// shard would diverge from the single-node oracle.
bool append_partition_rows(const std::string& body, const std::string& partition_id,
                           zetta::storage::JsonStore& store, std::uint64_t& rows,
                           std::string* error) {
    zetta::data::PartitionBodyReadResult read =
        zetta::data::read_partition_body(body, partition_id);
    if (!read.ok()) {
        if (error) *error = read.error;
        return false;
    }
    for (zetta::data::PartitionRow& row : read.rows) {
        store.add(std::move(row.id), std::move(row.doc));
        ++rows;
    }
    return true;
}

}  // namespace

LocalShardResult load_local_partitions(Node& node, const std::string& table,
                                       const DataFrameSchema& schema,
                                       compute::ComputeDevice& device,
                                       std::size_t chunk_rows) {
    LocalShardResult out;

    // **This walks the Node tree, so it must run on the thread that owns it.**
    // Calling it from a worker or an API handler does not crash and does not race
    // visibly: it stops, and the caller waits forever. That has cost two debugging
    // sessions, the churn executor's shard load (#952) and score_table (#956), each
    // found by sampling a hung process rather than by a failing test. Both survived
    // their tests because a test calls this from the owner thread, so the coverage
    // that should catch it structurally cannot.
    //
    // A guard here was tried and removed. There is no way from inside to tell the
    // bug from the sanctioned case: the testkit drives nodes from its own per-node
    // SPMD threads and arranges exclusive access itself, which is the documented
    // pre-gate contract on Node::run_on_owner, and it looks identical to an API
    // handler on the wrong thread. Failing on both would break a legitimate caller
    // to catch an illegitimate one.
    //
    // So the rule stays a convention rather than a check: **wrap this in
    // Node::run_on_owner unless you are the owner.** If a third instance of this
    // bug appears, the answer is probably a caller-supplied "I hold exclusive
    // access" flag rather than another attempt at inference.

    const std::string& self = node.node_id();

    // Enumerate this node's owned partitions of `table`, ascending by row range so
    // the loaded shard is deterministic / reproducible.
    std::vector<std::shared_ptr<zetta::data::DatasetPartition>> owned;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table) continue;
        const std::vector<std::string>& owners = partition->owner_nodes();
        if (std::find(owners.begin(), owners.end(), self) == owners.end()) continue;
        owned.push_back(partition);
    }
    std::sort(owned.begin(), owned.end(),
              [](const std::shared_ptr<zetta::data::DatasetPartition>& a,
                 const std::shared_ptr<zetta::data::DatasetPartition>& b) {
                  return a->row_range().start < b->row_range().start;
              });

    zetta::storage::JsonStore store;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition : owned) {
        if (partition->blob_ref().empty()) continue;  // unplaced / empty partition
        const std::optional<std::string> body =
            read_blob_body(node, partition->blob_ref());
        if (!body) {
            out.status = compute::ComputeStatus::backend_error(
                "load_local_partitions: owned partition '" + partition->id() +
                "' body (" + partition->blob_ref() + ") is not present locally");
            return out;
        }
        out.bytes_read += body->size();
        std::string error;
        if (!append_partition_rows(*body, partition->id(), store, out.rows_read,
                                   &error)) {
            out.status = compute::ComputeStatus::invalid_argument(
                "load_local_partitions: " + error);
            return out;
        }
        ++out.partitions_read;
    }

    DataFrameResult loaded = load_data_frame(store, schema, device, chunk_rows);
    out.frame = std::move(loaded.frame);
    out.status = loaded.status;
    return out;
}

}  // namespace zetta::ddf
