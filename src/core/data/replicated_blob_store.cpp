#include "zetta/data/replicated_blob_store.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_partition.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/sha256.hpp"

#include "../catalog.hpp"

namespace zetta::data {
namespace fs = std::filesystem;

namespace {

// Build the partition-owner index from a CRDT snapshot: maps a large object's
// body id (the bare uuid, no `lo:` prefix) to the node ids that own the
// partition backed by it. Reads the replicate-to-all partition catalog (`part:`
// keys — the same source `system.dataset_partitions` is computed from) through
// the validated boundary, so a malformed catalog entry is ignored, not trusted.
//
// Only partitions whose placement is recorded (a non-empty `owner_nodes`) and
// that name a backing blob (`blob_ref`) appear. An unplaced partition (empty
// `owner_nodes`) is absent, so its body falls back to the default
// replicate-to-all rule rather than being stranded with no owner to fetch it.
// `blob_ref` is normalized to the bare id, tolerating either the `lo:<uuid>`
// catalog-key form or a bare `<uuid>`.
std::unordered_map<std::string, std::vector<std::string>> partition_owner_index(
    const nlohmann::json& snapshot) {
    std::unordered_map<std::string, std::vector<std::string>> owners;
    catalog::for_each_with_prefix(
        snapshot, catalog::kPartitionPrefix,
        [&owners](const std::string& key, const nlohmann::json& value) {
            if (catalog::id_after(key, catalog::kPartitionPrefix).empty()) return;
            const std::optional<DatasetPartitionValue> partition =
                parse_dataset_partition_value(value);
            if (!partition || partition->owner_nodes.empty() ||
                partition->blob_ref.empty()) {
                return;
            }
            const std::string bare =
                catalog::id_after(partition->blob_ref, catalog::kLargeObjectPrefix);
            const std::string& large_object_id =
                bare.empty() ? partition->blob_ref : bare;
            std::vector<std::string>& nodes = owners[large_object_id];
            nodes.insert(nodes.end(), partition->owner_nodes.begin(),
                         partition->owner_nodes.end());
        });
    return owners;
}

}  // namespace

ReplicatedBlobStore::ReplicatedBlobStore(fs::path data_dir,
                                         zetta::cluster::ReplicationManager& replication)
    : data_dir_(std::move(data_dir)), replication_(replication) {}

bool ReplicatedBlobStore::node_holds_partition_blob(
    const std::unordered_map<std::string, std::vector<std::string>>& owners,
    const std::string& large_object_id) const {
    const auto it = owners.find(large_object_id);
    if (it == owners.end()) return true;  // not a placed partition body
    return std::find(it->second.begin(), it->second.end(),
                     replication_.node_id()) != it->second.end();
}

fs::path ReplicatedBlobStore::blob_path(const std::string& database_id,
                                        const std::string& id) const {
    return data_dir_ / database_id / "large_objects" / id / "blob";
}

fs::path ReplicatedBlobStore::weights_path(const std::string& model_id) const {
    return data_dir_ / model_id / "weights";
}

std::optional<ReplicatedBlobStore::BlobTarget> ReplicatedBlobStore::resolve(
    const nlohmann::json& snapshot, const std::string& id) const {
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
    // The shared immutable view (#1227): the `part:`, `lo:` and `model:` key
    // ranges are read in place, and nothing copies the catalogue.
    const std::shared_ptr<const nlohmann::json> snapshot = replication_.shared_snapshot();
    const std::unordered_map<std::string, std::vector<std::string>> owners =
        partition_owner_index(*snapshot);
    std::vector<std::string> missing;
    catalog::for_each_with_prefix(
        *snapshot, catalog::kLargeObjectPrefix,
        [&](const std::string& key, const nlohmann::json& value) {
            std::string id = catalog::id_after(key, catalog::kLargeObjectPrefix);
            if (id.empty() || !value.is_object() || !value.contains("db")) return;
            // Partition-aware placement: a partition's body is fetched only by
            // its owner nodes; a plain object (or unplaced partition) still
            // replicates to all.
            if (!node_holds_partition_blob(owners, id)) return;
            std::error_code ec;
            if (!fs::exists(blob_path(value["db"].get<std::string>(), id), ec)) {
                missing.push_back(std::move(id));
            }
        });
    catalog::for_each_with_prefix(
        *snapshot, catalog::kModelPrefix,
        [&](const std::string& key, const nlohmann::json& value) {
            std::string model_id = catalog::id_after(key, catalog::kModelPrefix);
            if (model_id.empty() || !value.is_object() || !value.contains("weights") ||
                !value["weights"].is_object()) {
                return;
            }
            std::error_code ec;
            if (!fs::exists(weights_path(model_id), ec)) {
                missing.push_back(std::move(model_id));
            }
        });
    return missing;
}

std::optional<std::string> ReplicatedBlobStore::read_blob(
    const std::string& id) const {
    const std::optional<BlobTarget> target = resolve(*replication_.shared_snapshot(), id);
    if (!target) return std::nullopt;
    std::ifstream in(target->path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool ReplicatedBlobStore::read_blob_range(const std::string& id, std::uint64_t offset,
                                          std::uint64_t length, std::string& out,
                                          std::uint64_t& size) const {
    const std::optional<BlobTarget> target = resolve(*replication_.shared_snapshot(), id);
    if (!target) return false;
    std::error_code ec;
    const std::uintmax_t whole = fs::file_size(target->path, ec);
    if (ec || offset > whole) return false;
    std::ifstream in(target->path, std::ios::binary);
    if (!in) return false;
    size = whole;
    const std::uint64_t take = std::min<std::uint64_t>(length, whole - offset);
    out.assign(static_cast<std::size_t>(take), '\0');
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(out.data(), static_cast<std::streamsize>(take));
    return static_cast<std::uint64_t>(in.gcount()) == take;
}

bool ReplicatedBlobStore::store_blob(const std::string& id,
                                     const std::string& bytes) {
    std::optional<BlobTarget> target;
    {
        const std::shared_ptr<const nlohmann::json> snapshot = replication_.shared_snapshot();
        target = resolve(*snapshot, id);
        if (!target) return false;  // no metadata for this id — refuse
        // Partition-aware placement: only a partition's owner nodes hold its body.
        // Defense-in-depth — a non-owner never requests it, so this rejects only an
        // unsolicited push, keeping a partition body off non-owners.
        if (!node_holds_partition_blob(partition_owner_index(*snapshot), id)) {
            return false;
        }
    }
    // Verify against the replicated metadata; a mismatch is dropped, not stored.
    if (target->has_size && bytes.size() != target->size) return false;
    // Only enforce the hash when one was recorded. Objects imported before
    // SHA-256 existed carry an empty hash — verify by size and accept (matching
    // the preflight check, which also skips an empty hash).
    if (target->has_sha256 && !target->sha256.empty() &&
        zetta::util::sha256_hex(bytes) != target->sha256) {
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

}  // namespace zetta::data