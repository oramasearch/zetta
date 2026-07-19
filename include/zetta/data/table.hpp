#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/data/partitioner.hpp"
#include "zetta/data/replication_strategy.hpp"
#include "zetta/storage/engine.hpp"

namespace zetta::cluster { class ReplicationManager; }

namespace zetta::data {

// folder (named by a UUID) via a JSONL storage engine. The folder name is the
// id; the human name lives in table.json beside the row chunks.
//
// With a ReplicationManager, each inserted row is also written to the replicated
// CRDT (capture); `materialize_row` is the reverse path used by reconciliation
// to apply a row that arrived from a peer, without re-capturing it.
//
// A **partition-only** table (#833) suppresses that capture entirely — see
// `partition_only()` below.
class Table {
public:
    Table(std::string id, std::string name, std::filesystem::path dir,
          zetta::cluster::ReplicationManager* replication = nullptr,
          ReplicationStrategy replication_strategy = {},
          bool partition_only = false);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::filesystem::path& dir() const { return dir_; }

    // How this table's replicated state is laid out across the cluster (ENT-35).
    // Defaults to ReplicateToAll — the behavior every table has today. Recorded +
    // persisted + replicated; the partitioned-placement engine that acts on a
    // non-default strategy is a later slice. See replication_strategy.hpp.
    const ReplicationStrategy& replication_strategy() const {
        return replication_strategy_;
    }

    // **Partition-only** (#833 section 4): this table's rows are NOT captured
    // into the replicated document. Its authoritative data is the **partition
    // bodies** the partitioned write path emitted plus the replicate-to-all
    // partition catalog that says who owns them; the local store holds only the
    // slice this node produced, as the ingest's staging buffer.
    //
    // Why it exists: a distributed ingest has each node land the slice it pulled
    // as partitions *it* owns. Capturing those rows would replicate every one of
    // them back to every peer — the rows would cross the wire a second time,
    // which is exactly what tiling the fetch was for.
    //
    // Why a non-capturing write is still durable: reconcile prunes databases,
    // tables and large objects, but **not rows** — rows are only additively
    // materialized from the document, so a row present locally and absent from
    // the CRDT is never pruned. `JsonlStore` reloads from disk in its
    // constructor, so the local slice returns on restart independently of the
    // CRDT, and the partition bodies are large objects held by their owners.
    //
    // Consequences, both handled by the engine: `system.datasets` counts rows
    // from the partition catalog's row ranges rather than `store().size()`
    // (`Node::datasets()`), and a `SELECT` over such a table is answered by
    // scatter-gather across the partition owners rather than from the local
    // store (`zetta::query::DistributedSelect`).
    //
    // Recorded in table.json and in the `tbl:` CRDT value, so every node agrees
    // on how the table must be read. Fixed at create time; there is no update
    // path (flipping it would strand rows on one side or the other).
    bool partition_only() const { return partition_only_; }

    zetta::storage::StorageEngine& store() { return *store_; }
    const zetta::storage::StorageEngine& store() const { return *store_; }

    // Insert a row (persisted immediately + captured into the CRDT); returns the
    // new row count. The row's id is generated here and used as its CRDT key.
    // On a partition_only() table the capture is skipped — the row lands in the
    // local store only.
    std::size_t add_row(nlohmann::json doc);

    // Update a row in place by id: overwrites its CRDT value + the local store.
    // No-op if the id is absent. Backs SQL UPDATE. No capture on a
    // partition_only() table.
    void update_row(const std::string& row_id, nlohmann::json doc);

    // Delete a row by id: writes a TOMBSTONE to the CRDT (`{tbl, deleted:true}`, so
    // the removal replicates as a value change a peer can apply) and drops the row
    // from the local store. Backs SQL DELETE. No tombstone on a partition_only()
    // table — nothing was ever captured to tombstone.
    void delete_row(const std::string& row_id);

    // Apply a replicated row (from a peer) to the local store: adds it, or updates
    // it in place if the id already exists (upsert). Does NOT write back to the
    // CRDT. Returns whether a NEW row was added (false = updated or unchanged).
    bool materialize_row(std::string row_id, nlohmann::json doc);

    // Apply a replicated row tombstone (from a peer): drop the row if present. Does
    // NOT write back to the CRDT.
    void materialize_row_delete(const std::string& row_id);

    std::size_t row_count() const;

    // Configure this table's partitioner — the per-table policy that maps a row
    // to a partition and a partition to its owner node(s) (the Partitioner seam,
    // ADR-31 / ENT-37). null = not partitioned (the default). This is in-memory
    // configuration only in this slice; persisting/replicating the selection and
    // the partition placements is the partition catalog (#510) and the
    // partitioned write path (#511). See docs/data/partitioner.md.
    void set_partitioner(std::shared_ptr<Partitioner> partitioner);

    // The configured partitioner, or an empty handle when the table is not
    // partitioned.
    std::shared_ptr<Partitioner> partitioner() const { return partitioner_; }

    // Write this table's metadata (id, name) to dir/table.json.
    void persist_meta() const;

private:
    std::string id_;
    std::string name_;
    std::filesystem::path dir_;
    std::unique_ptr<zetta::storage::StorageEngine> store_;
    zetta::cluster::ReplicationManager* replication_;  // null = not replicated
    std::shared_ptr<Partitioner> partitioner_;  // null = not partitioned
    ReplicationStrategy replication_strategy_;  // ENT-35; default ReplicateToAll
    bool partition_only_ = false;               // #833; rows never captured
};

}  // namespace zetta::data