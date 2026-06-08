#include "zetta/replicated_blob_store.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/replication.hpp"
#include "zetta/sha256.hpp"

#include "catalog.hpp"

namespace zetta {
namespace fs = std::filesystem;

ReplicatedBlobStore::ReplicatedBlobStore(fs::path data_dir,
                                         ReplicationManager& replication)
    : data_dir_(std::move(data_dir)), replication_(replication) {}

fs::path ReplicatedBlobStore::blob_path(const std::string& database_id,
                                        const std::string& id) const {
    return data_dir_ / database_id / "large_objects" / id / "blob";
}

std::vector<std::string> ReplicatedBlobStore::missing_blobs() const {
    const nlohmann::json snapshot = replication_.snapshot();
    std::vector<std::string> missing;
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kLargeObjectPrefix);
        if (id.empty() || !value.is_object() || !value.contains("db")) continue;
        const std::string database_id = value["db"].get<std::string>();
        std::error_code ec;
        if (!fs::exists(blob_path(database_id, id), ec)) {
            missing.push_back(std::move(id));
        }
    }
    return missing;
}

std::optional<std::string> ReplicatedBlobStore::read_blob(
    const std::string& id) const {
    const nlohmann::json snapshot = replication_.snapshot();
    const auto it = snapshot.find(catalog::large_object_key(id));
    if (it == snapshot.end() || !it->is_object() || !it->contains("db")) {
        return std::nullopt;
    }
    std::ifstream in(blob_path((*it)["db"].get<std::string>(), id),
                     std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool ReplicatedBlobStore::store_blob(const std::string& id,
                                     const std::string& bytes) {
    const nlohmann::json snapshot = replication_.snapshot();
    const auto it = snapshot.find(catalog::large_object_key(id));
    if (it == snapshot.end() || !it->is_object() || !it->contains("db")) {
        return false;  // no metadata for this object — refuse
    }
    const nlohmann::json& meta = *it;
    // Verify against the replicated metadata; a mismatch is dropped, not stored.
    if (meta.contains("size") &&
        bytes.size() != meta["size"].get<std::uintmax_t>()) {
        return false;
    }
    // Only enforce the hash when one was recorded. Objects imported before
    // SHA-256 existed carry an empty hash — verify by size and accept (matching
    // the preflight check, which also skips an empty hash).
    if (meta.contains("sha256")) {
        const std::string expected = meta["sha256"].get<std::string>();
        if (!expected.empty() && sha256_hex(bytes) != expected) {
            return false;
        }
    }
    const fs::path path = blob_path(meta["db"].get<std::string>(), id);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) return false;
    }
    fs::rename(tmp, path, ec);  // atomic publish
    return !ec;
}

}  // namespace zetta
