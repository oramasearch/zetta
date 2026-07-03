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

// Append a partition body's rows to `store`. The body is JSON Lines, one
// `{"_zid": <id>, "doc": <row>}` object per line (the JsonlStore on-disk shape the
// partitioned write path emits) — read the same tolerant way JsonlStore::load
// does, but **fail fast** on a line that is not valid JSON or not a JSON object,
// since a silently-dropped row would diverge from the single-node oracle. Returns
// false (and a reason in `error`) on the first malformed line.
bool append_partition_rows(const std::string& body, const std::string& partition_id,
                           zetta::storage::JsonStore& store, std::uint64_t& rows,
                           std::string* error) {
    std::istringstream in(body);
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;
        nlohmann::json parsed =
            nlohmann::json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded()) {
            if (error) {
                *error = "partition '" + partition_id + "' line " +
                         std::to_string(line_no) + " is not valid JSON";
            }
            return false;
        }
        // Current format: { "_zid": id, "doc": data }. A bare object is read as the
        // row itself (legacy / tolerant, matching JsonlStore); only the `doc` is
        // needed for a DataFrame, so a synthetic id suffices when one is absent.
        std::string id;
        nlohmann::json doc;
        if (parsed.is_object() && parsed.contains("_zid") && parsed.contains("doc")) {
            const nlohmann::json& zid = parsed["_zid"];
            id = zid.is_string() ? zid.get<std::string>()
                                 : partition_id + "#" + std::to_string(line_no);
            doc = std::move(parsed["doc"]);
        } else if (parsed.is_object()) {
            id = partition_id + "#" + std::to_string(line_no);
            doc = std::move(parsed);
        } else {
            if (error) {
                *error = "partition '" + partition_id + "' line " +
                         std::to_string(line_no) + " is not a JSON object";
            }
            return false;
        }
        store.add(std::move(id), std::move(doc));
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
