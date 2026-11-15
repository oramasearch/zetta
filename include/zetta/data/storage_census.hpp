// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// What a node holds on its disk, by kind (docs/data/storage-census.md): the bytes of every file
// under its data directory, each counted once in the kind its top-level entry is. A host (lake,
// which bills a workspace's storage by kind) reads it through the `storage_usage` capability.
//
// The kinds, from the data directory's layout (node.cpp's load_* passes):
//   datasets   a database's directory (`database.json`) but its `large_objects/`, and dataset
//              partitions (`partition.json`)
//   files      a database's `large_objects/` (files and blobs)
//   models     a model's directory (`model.json`, weights included) and `checkpoints/`
//   indexes    an index's directory (`index.json`)
//   runs       a run's or study's directory (`run.json`, `study.json`) and `experiments/`
//   buckets    `objects/`, the object store's buckets; `buckets` gives each bucket's bytes
//   streams    `pubsub/`, the pub/sub topics' logs and spill
//   records    `records/`, the record log (records and labelled metrics)
//   catalogue  every other entity's directory (projects, jobs, deployments, principals, ...)
//   system     everything else: the audit trail, the ledger, security material, the
//              replication state, crash reports, the node id
namespace zetta::data {

struct StorageCensus {
    std::uint64_t total_bytes = 0;
    // Every kind of storage_kinds(), 0 when absent.
    std::map<std::string, std::uint64_t> kinds;
    // Each bucket under `objects/` and its bytes.
    std::map<std::string, std::uint64_t> buckets;
};

// The kinds, in the order the census reports them.
const std::vector<std::string_view>& storage_kinds();

// Walks `data_dir` (symbolic links are not followed; an unreadable entry counts nothing) and
// classifies each file. Blocking filesystem work: run it off the owner loop.
StorageCensus measure_storage(const std::filesystem::path& data_dir);

}  // namespace zetta::data
