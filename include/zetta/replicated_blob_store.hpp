#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "zetta/blob_store.hpp"

namespace zetta {

class ReplicationManager;

// A BlobStore that works purely from the replicated CRDT metadata + the on-disk
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
class ReplicatedBlobStore final : public BlobStore {
public:
    ReplicatedBlobStore(std::filesystem::path data_dir,
                        ReplicationManager& replication);

    std::vector<std::string> missing_blobs() const override;
    std::optional<std::string> read_blob(const std::string& id) const override;
    bool store_blob(const std::string& id, const std::string& bytes) override;

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
    std::optional<BlobTarget> resolve(const std::string& id) const;

    std::filesystem::path data_dir_;
    ReplicationManager& replication_;
};

}  // namespace zetta
