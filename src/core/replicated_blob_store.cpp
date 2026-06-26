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

fs::path ReplicatedBlobStore::weights_path(const std::string& model_id) const {
    return data_dir_ / model_id / "weights";
}

std::optional<ReplicatedBlobStore::BlobTarget> ReplicatedBlobStore::resolve(
    const std::string& id) const {
    const nlohmann::json snapshot = replication_.snapshot();
    // Build a target from a body path + the metadata to verify against. `size` /
    // `sha256` are optional — an object imported before SHA-256 existed carries
    // an empty hash, verified by size only.
    const auto make_target = [](fs::path path, const nlohmann::json& meta) {
        BlobTarget target;
        target.path = std::move(path);
        if (meta.contains("size")) {
            target.size = meta["size"].get<std::uintmax_t>();
            target.has_size = true;
        }
        if (meta.contains("sha256")) {
            target.sha256 = meta["sha256"].get<std::string>();
            target.has_sha256 = true;
        }
        return target;
    };
    // Large object: lo:<id> -> { db, name, size, sha256 }.
    if (const auto it = snapshot.find(catalog::large_object_key(id));
        it != snapshot.end() && it->is_object() && it->contains("db")) {
        return make_target(blob_path((*it)["db"].get<std::string>(), id), *it);
    }
    // Model weights: model:<id> -> { ..., weights: { size, sha256 } }.
    if (const auto it = snapshot.find(catalog::model_key(id));
        it != snapshot.end() && it->is_object() && it->contains("weights") &&
        (*it)["weights"].is_object()) {
        return make_target(weights_path(id), (*it)["weights"]);
    }
    return std::nullopt;  // no metadata for this id — not ours to serve
}

std::vector<std::string> ReplicatedBlobStore::missing_blobs() const {
    const nlohmann::json snapshot = replication_.snapshot();
    std::vector<std::string> missing;
    for (const auto& [key, value] : snapshot.items()) {
        std::error_code ec;
        if (std::string id = catalog::id_after(key, catalog::kLargeObjectPrefix);
            !id.empty() && value.is_object() && value.contains("db")) {
            if (!fs::exists(blob_path(value["db"].get<std::string>(), id), ec)) {
                missing.push_back(std::move(id));
            }
        } else if (std::string model_id =
                       catalog::id_after(key, catalog::kModelPrefix);
                   !model_id.empty() && value.is_object() &&
                   value.contains("weights") && value["weights"].is_object()) {
            if (!fs::exists(weights_path(model_id), ec)) {
                missing.push_back(std::move(model_id));
            }
        }
    }
    return missing;
}

std::optional<std::string> ReplicatedBlobStore::read_blob(
    const std::string& id) const {
    const std::optional<BlobTarget> target = resolve(id);
    if (!target) return std::nullopt;
    std::ifstream in(target->path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool ReplicatedBlobStore::store_blob(const std::string& id,
                                     const std::string& bytes) {
    const std::optional<BlobTarget> target = resolve(id);
    if (!target) return false;  // no metadata for this id — refuse
    // Verify against the replicated metadata; a mismatch is dropped, not stored.
    if (target->has_size && bytes.size() != target->size) return false;
    // Only enforce the hash when one was recorded. Objects imported before
    // SHA-256 existed carry an empty hash — verify by size and accept (matching
    // the preflight check, which also skips an empty hash).
    if (target->has_sha256 && !target->sha256.empty() &&
        sha256_hex(bytes) != target->sha256) {
        return false;
    }
    const fs::path& path = target->path;
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
