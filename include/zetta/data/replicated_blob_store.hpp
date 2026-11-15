#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/blob_store.hpp"

namespace zetta::cluster { class ReplicationManager; }

namespace zetta::data {

// layout — it touches neither the Node tree nor its in-memory large-object
// stores, so the cluster's network thread can call it without data races.
//
// It carries two kinds of body over the one seam, both keyed by UUID:
//   - large objects — bodies at <data_dir>/<db-uuid>/large_objects/<lo-uuid>/blob,
//     where the db-uuid comes from the object's `lo:` metadata in the document.
//   - model weights — bodies at <data_dir>/<model-uuid>/weights, for any
//     `model:` entry carrying a `weights:{size,sha256}` descriptor.
// "Missing" = the document has the metadata but the body file isn't on disk.
// `store_blob` verifies length + SHA-256 against that metadata before writing.
//
// Partition-aware placement (issue #515). A large-object body that is the
// backing blob of a dataset partition (a `part:` catalog entry's `blob_ref`,
// see dataset_partition.hpp) is held **only by that partition's owner nodes**
// (its `owner_nodes`, recorded by placement and visible globally via
// `system.dataset_partitions`) rather than replicated to all: a non-owner does
// not list it in `missing_blobs()` (so it never fetches it) and refuses it in
// `store_blob()`. With a replication factor of `rf` (rf owner nodes) the body
// therefore lives on exactly `rf` nodes. A plain large object, an unplaced
// partition (empty `owner_nodes`), and model weights all keep the default
// replicate-to-all behavior.
class ReplicatedBlobStore final : public BlobStore {
public:
    ReplicatedBlobStore(std::filesystem::path data_dir,
                        zetta::cluster::ReplicationManager& replication);

    std::vector<std::string> missing_blobs() const override;
    std::optional<std::string> read_blob(const std::string& id) const override;
    bool store_blob(const std::string& id, const std::string& bytes) override;

    // A slice [offset, offset + length) of a body this node holds, and the
    // body's whole size: what a member taking a backup snapshot fetches a
    // slice at a time (zetta#1520). False when the body is not held here or
    // `offset` is past its end; a slice past the end is cut at the end.
    bool read_blob_range(const std::string& id, std::uint64_t offset, std::uint64_t length,
                         std::string& out, std::uint64_t& size) const;

private:
    // Where a large object's body / a model's weights body lives on disk.
    std::filesystem::path blob_path(const std::string& database_id,
                                    const std::string& id) const;
    std::filesystem::path weights_path(const std::string& model_id) const;

    // The resolved transfer target for a blob id: where its body belongs plus the
    // size/hash to verify it against. Either a large object (`lo:`) or a model's
    // weights (`model:`) — looked up from the document by id.
    struct BlobTarget {
        std::filesystem::path path;
        std::uintmax_t size = 0;
        bool has_size = false;
        std::string sha256;
        bool has_sha256 = false;
    };
    std::optional<BlobTarget> resolve(const nlohmann::json& snapshot,
                                      const std::string& id) const;

    // Partition placement: should this node hold `large_object_id`'s body? True
    // unless the blob backs a partition (a `part:` entry's `blob_ref`) whose
    // recorded `owner_nodes` set is non-empty and does not include this node.
    // `owners` is the partition-owner index built once per call from `snapshot`.
    bool node_holds_partition_blob(
        const std::unordered_map<std::string, std::vector<std::string>>& owners,
        const std::string& large_object_id) const;

    std::filesystem::path data_dir_;
    zetta::cluster::ReplicationManager& replication_;
};

}  // namespace zetta::data