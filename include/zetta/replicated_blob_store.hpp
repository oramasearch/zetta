#pragma once

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
// Bodies live at <data_dir>/<db-uuid>/large_objects/<lo-uuid>/blob, where the
// db-uuid comes from the object's `lo:` metadata in the document. "Missing" =
// the document has the metadata but that blob file isn't on disk. `store_blob`
// verifies length + SHA-256 against the metadata before writing.
class ReplicatedBlobStore final : public BlobStore {
public:
    ReplicatedBlobStore(std::filesystem::path data_dir,
                        ReplicationManager& replication);

    std::vector<std::string> missing_blobs() const override;
    std::optional<std::string> read_blob(const std::string& id) const override;
    bool store_blob(const std::string& id, const std::string& bytes) override;

private:
    std::filesystem::path blob_path(const std::string& database_id,
                                    const std::string& id) const;

    std::filesystem::path data_dir_;
    ReplicationManager& replication_;
};

}  // namespace zetta
