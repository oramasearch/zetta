// SPDX-License-Identifier: MIT

#include "zetta/data/storage_census.hpp"

#include <system_error>

namespace zetta::data {
namespace {

namespace fs = std::filesystem;

// The bytes of every regular file under `path` (or of `path` itself when it is one).
std::uint64_t bytes_under(const fs::path& path) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error) return 0;
    if (fs::is_regular_file(status)) {
        const std::uintmax_t size = fs::file_size(path, error);
        return error ? 0 : static_cast<std::uint64_t>(size);
    }
    if (!fs::is_directory(status)) return 0;
    std::uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(
             path, fs::directory_options::skip_permission_denied, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !it->is_symlink(entry_error)) {
            const std::uintmax_t size = it->file_size(entry_error);
            if (!entry_error) total += static_cast<std::uint64_t>(size);
        }
    }
    return total;
}

bool has(const fs::path& directory, std::string_view marker) {
    std::error_code error;
    return fs::is_regular_file(directory / marker, error);
}

// The kind of an entity directory, by the marker file the node's load_* pass reads.
std::string_view entity_kind(const fs::path& directory) {
    if (has(directory, "database.json") || has(directory, "partition.json")) return "datasets";
    if (has(directory, "model.json")) return "models";
    if (has(directory, "index.json")) return "indexes";
    if (has(directory, "run.json") || has(directory, "study.json")) return "runs";
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (entry.path().extension() == ".json" && entry.is_regular_file(error)) {
            return "catalogue";
        }
    }
    return "system";
}

}  // namespace

const std::vector<std::string_view>& storage_kinds() {
    static const std::vector<std::string_view> kinds{
        "datasets", "files",   "models",    "indexes", "runs",
        "buckets",  "streams", "records",   "catalogue", "system"};
    return kinds;
}

StorageCensus measure_storage(const fs::path& data_dir) {
    StorageCensus census;
    for (const std::string_view kind : storage_kinds()) census.kinds[std::string(kind)] = 0;
    const auto add = [&census](std::string_view kind, std::uint64_t bytes) {
        census.kinds[std::string(kind)] += bytes;
        census.total_bytes += bytes;
    };
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(data_dir, error)) {
        const fs::path& path = entry.path();
        const std::string name = path.filename().string();
        std::error_code entry_error;
        if (entry.is_symlink(entry_error) || !entry.is_directory(entry_error)) {
            add("system", bytes_under(path));
            continue;
        }
        if (name == "objects") {
            for (const auto& bucket : fs::directory_iterator(path, entry_error)) {
                const std::uint64_t bytes = bytes_under(bucket.path());
                std::error_code bucket_error;
                if (bucket.is_directory(bucket_error) && !bucket.is_symlink(bucket_error)) {
                    census.buckets[bucket.path().filename().string()] += bytes;
                }
                add("buckets", bytes);
            }
        } else if (name == "pubsub") {
            add("streams", bytes_under(path));
        } else if (name == "records") {
            add("records", bytes_under(path));
        } else if (name == "checkpoints") {
            add("models", bytes_under(path));
        } else if (name == "experiments") {
            add("runs", bytes_under(path));
        } else if (name == "audit" || name == "ledger" || name == "security" ||
                   name == "crashes") {
            add("system", bytes_under(path));
        } else {
            const std::string_view kind = entity_kind(path);
            if (kind == "datasets" && has(path, "database.json")) {
                const std::uint64_t files = bytes_under(path / "large_objects");
                add("files", files);
                add("datasets", bytes_under(path) - files);
            } else {
                add(kind, bytes_under(path));
            }
        }
    }
    return census;
}

}  // namespace zetta::data
