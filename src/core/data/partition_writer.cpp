#include "zetta/data/partition_writer.hpp"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/data/table.hpp"

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
                                       const std::vector<std::string>& members) {
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
        // without opening it (the read path still sniffs the bytes).
        value.stats = nlohmann::json{{"partition", partition},
                                     {"rows", ordinals.size()},
                                     {"format", to_string(serialized.format)}};

        std::shared_ptr<DatasetPartition> recorded =
            node.create_dataset_partition(std::move(value));
        if (!recorded) {
            result.error = "failed to record partition " +
                           std::to_string(partition) + " in the catalog";
            return result;
        }

        result.partitions.push_back(std::move(recorded));
        offset += ordinals.size();
        result.rows_written += ordinals.size();
    }

    return result;
}

}  // namespace zetta::data