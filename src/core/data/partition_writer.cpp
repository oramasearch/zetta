#include "zetta/data/partition_writer.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_columns.hpp"
#include "zetta/data/dataset_manifest.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/data/table.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::data {
namespace {

// The default surrogate field a RoundRobinPartitioner reads; the write path stamps
// it (= the row's table ordinal) when a row doesn't already carry it, so even
// distribution works without the caller pre-stamping. Kept in sync with
// RoundRobinPartitioner's default; harmlessly ignored by other partitioners.
constexpr const char* kSurrogateField = "_seq";

// A partition's rows, in the order its ordinals were bucketed — the input the
// body serializer turns into Parquet (or, for rows Parquet cannot express, JSON
// Lines). An unreadable row fails the write: a partition silently short of rows
// would diverge from the table it was written from.
std::string collect_partition_rows(const zetta::storage::StorageEngine& store,
                                   const std::vector<std::size_t>& ordinals,
                                   std::vector<PartitionRow>& rows) {
    rows.clear();
    rows.reserve(ordinals.size());
    for (const std::size_t ordinal : ordinals) {
        const zetta::storage::ParquetLazyPtr row = store.at(ordinal);
        if (!row) {
            return "row " + store.id_at(ordinal) + " could not be read: " + row.error();
        }
        rows.push_back(PartitionRow{store.id_at(ordinal), *row});
    }
    return {};
}

}  // namespace

PartitionWriteResult write_partitioned(Node& node, Database& database,
                                       Table& table,
                                       const std::vector<std::string>& members,
                                       bool record_manifest) {
    PartitionWriteResult result;

    const std::shared_ptr<Partitioner> partitioner = table.partitioner();
    if (!partitioner) {
        result.error = "table '" + table.name() + "' is not partitioned";
        return result;
    }

    // Placement is computed over the cluster membership; default to this node so a
    // standalone write still records a concrete owner.
    const std::vector<std::string> placement_members =
        members.empty() ? std::vector<std::string>{node.node_id()} : members;

    // Bucket each row's ordinal under its assigned partition. std::map keeps the
    // partition ids ascending, which is the order the cumulative row ranges and the
    // catalog entries are emitted in.
    const zetta::storage::StorageEngine& store = table.store();
    std::map<PartitionId, std::vector<std::size_t>> buckets;
    for (std::size_t ordinal = 0; ordinal < store.size(); ++ordinal) {
        // Stamp the monotonic surrogate (the row ordinal) onto the assignment
        // record when absent — enables RoundRobinPartitioner without mutating the
        // stored/blob row. assign_partition is pure, so this copy is assignment-only.
        const zetta::storage::ParquetLazyPtr stored = store.at(ordinal);
        if (!stored) {
            result.error = "row " + store.id_at(ordinal) +
                           " could not be read: " + stored.error();
            return result;
        }
        nlohmann::json assignment = *stored;
        if (assignment.is_object() && !assignment.contains(kSurrogateField)) {
            assignment[kSurrogateField] = ordinal;
        }
        const PartitionId partition = partitioner->assign_partition(assignment);
        buckets[partition].push_back(ordinal);
    }

    // Serialize → store as a blob → record placement, per non-empty partition.
    std::uint64_t offset = 0;  // cumulative row count = the next partition's start
    std::vector<PartitionRow> rows;
    for (const auto& [partition, ordinals] : buckets) {
        if (std::string error = collect_partition_rows(store, ordinals, rows);
            !error.empty()) {
            result.error = std::move(error);
            return result;
        }
        const PartitionBodyWriteResult serialized = serialize_partition_body(rows);
        if (!serialized.ok()) {
            result.error = "failed to serialize partition " +
                           std::to_string(partition) + ": " + serialized.error;
            return result;
        }
        const std::string name = "partition-" + table.id() + "-" +
                                 std::to_string(partition) +
                                 std::string(body_extension(serialized.format));
        const std::optional<LargeObject> body =
            database.large_objects().import_bytes(name, serialized.bytes);
        if (!body) {
            result.error = "failed to store partition " +
                           std::to_string(partition) + " body";
            return result;
        }

        DatasetPartitionValue value;
        value.table = table.id();
        value.blob_ref = body->id;
        value.row_range = {offset, offset + ordinals.size()};
        value.owner_nodes = partitioner->place_partition(partition, placement_members);
        // The body's format is recorded, so a reader knows what it is holding
        // without opening it (the read path still sniffs the bytes). The column
        // names go in beside it: a partition-only table (#833) has no local copy
        // of the dataset on a node that did not produce it, so system.datasets
        // reads the schema off the replicate-to-all catalog rather than off a
        // store that is empty there.
        nlohmann::json columns = nlohmann::json::array();
        // Each column's JSON type and null count over this partition's rows
        // (#1021), so get_dataset can type a partition-only dataset's columns
        // and say how many values are null without reading a body.
        nlohmann::json column_stats;
        {
            std::unordered_set<std::string> seen;
            std::vector<std::string> names;
            ColumnProfile profile;
            for (const PartitionRow& row : rows) {
                profile.add_row(row.doc);
                if (!row.doc.is_object()) continue;
                for (auto field = row.doc.begin(); field != row.doc.end();
                     ++field) {
                    if (seen.insert(field.key()).second) {
                        columns.push_back(field.key());
                        names.push_back(field.key());
                    }
                }
            }
            column_stats = profile.stats(names);
        }
        // The blob's OWN schema — column names with the physical type each was
        // written as — and its byte size, recorded beside the row count (#837).
        // Together with `columns` and `row_range` that is everything a planner
        // needs: a node can plan, prune, and report `system.datasets` for a
        // partition it does not hold a byte of. The CRDT replicates the metadata
        // describing these blobs; it never replicates the blobs.
        nlohmann::json schema = nlohmann::json::array();
        for (const PartitionBodyColumn& column : serialized.schema) {
            schema.push_back(nlohmann::json{{"name", column.name},
                                            {"type", column.type},
                                            {"optional", column.optional}});
        }
        // The partitioner that PLACED this entry is recorded beside the rest.
        // The between-jobs rebalance reads it: recomputing a hash is right for a
        // rendezvous-placed partition and wrong for a fetch-affinity one, whose
        // owner is "the node holding the bytes", not a hash of its index.
        value.stats = nlohmann::json{{"partition", partition},
                                     {"rows", ordinals.size()},
                                     {"format", to_string(serialized.format)},
                                     {"placement", partitioner->name()},
                                     {"columns", std::move(columns)},
                                     {"schema", std::move(schema)},
                                     {"column_stats", std::move(column_stats)},
                                     {"bytes", serialized.bytes.size()}};

        std::shared_ptr<DatasetPartition> recorded =
            node.create_dataset_partition(std::move(value));
        if (!recorded) {
            result.error = "failed to record partition " +
                           std::to_string(partition) + " in the catalog";
            return result;
        }

        result.bytes_written += serialized.bytes.size();
        result.partitions.push_back(std::move(recorded));
        offset += ordinals.size();
        result.rows_written += ordinals.size();
    }

    // The manifest (#837): how many partitions this dataset HAS, recorded once
    // the write has finished, so a reader can tell "I can see 6 of 8" from
    // "there are 6". Suppressed for one tile of a cluster-wide write — the
    // manifest must describe the whole dataset, and only the coordinator knows
    // that total (the ingest's allreduce), so it writes the manifest itself.
    if (record_manifest) {
        DatasetManifestValue manifest;
        manifest.table = table.id();
        manifest.partitions = result.partitions.size();
        manifest.rows = result.rows_written;
        manifest.bytes = result.bytes_written;
        for (const std::shared_ptr<DatasetPartition>& recorded : result.partitions) {
            manifest.partition_ids.push_back(recorded->id());
        }
        manifest.created = zetta::util::now_iso8601();
        table.set_partition_manifest(std::move(manifest));
    }

    return result;
}

}  // namespace zetta::data