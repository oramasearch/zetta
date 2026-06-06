#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zetta {

// Metadata for one stored file ("large object", after Postgres' LO API).
struct LargeObject {
    std::string id;        // UUIDv4; also its folder name on disk
    std::string name;      // original filename
    std::uintmax_t size;   // bytes
};

// Stores arbitrary files as binary blobs in a directory, one folder per object
// (named by a UUID) holding `blob` (raw bytes) + `meta.json`. This is zetta's
// analog of Postgres' large object API; a Database owns one.
//
// (Whole-file in memory for now — import/read/export load the full blob; a
// streaming/chunked path can come later.)
class LargeObjectStore {
public:
    explicit LargeObjectStore(std::filesystem::path dir);

    // Import a file as a new large object (analogous to lo_import). Returns the
    // assigned metadata, or nullopt if the source can't be read / written.
    std::optional<LargeObject> import_file(const std::filesystem::path& src);

    // Write a stored object's bytes to `dst` (lo_export). False if id unknown
    // or the write fails.
    bool export_file(const std::string& id,
                     const std::filesystem::path& dst) const;

    // Read a stored object's raw bytes (lo_read). Nullopt if id unknown.
    std::optional<std::string> read(const std::string& id) const;

    // Delete a stored object (lo_unlink). True if one was removed.
    bool unlink(const std::string& id);

    // All objects, sorted by name.
    std::vector<LargeObject> list() const;
    std::size_t count() const { return objects_.size(); }

private:
    void load();
    const LargeObject* find(const std::string& id) const;
    std::filesystem::path object_dir(const std::string& id) const;

    std::filesystem::path dir_;
    std::vector<LargeObject> objects_;
};

}  // namespace zetta
