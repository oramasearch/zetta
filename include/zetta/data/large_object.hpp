// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zetta::cluster { class ReplicationManager; }

namespace zetta::data {

// metadata is what replicates (in the CRDT); the body transfers separately and
// is verified against `size` + `sha256`.
struct LargeObject {
    std::string id;            // UUIDv4; also its folder name on disk
    std::string name;          // original filename
    std::uintmax_t size = 0;   // bytes
    std::string sha256;        // lowercase-hex SHA-256 of the body
    bool body_present = true;  // is the body on disk here yet? (false until synced)
};

// Stores arbitrary files as binary blobs in a directory, one folder per object
// (named by a UUID) holding `blob` (raw bytes) + `meta.json`. zetta's analog of
// Postgres' large object API; a Database owns one.
//
// With a ReplicationManager, the object's **metadata** (id, name, size, sha256)
// is captured into the replicated CRDT on import; the body is transferred
// peer-to-peer by the cluster. `materialize_object` is the reverse path used by
// reconciliation to register metadata that arrived from a peer (the body may
// still be syncing — `body_present` is false until it lands).
class LargeObjectStore {
public:
    explicit LargeObjectStore(std::filesystem::path dir,
                              std::string database_id = {},
                              zetta::cluster::ReplicationManager* replication = nullptr);

    // Import a file as a new large object (analogous to lo_import). Returns the
    // assigned metadata, or nullopt if the source can't be read / written.
    std::optional<LargeObject> import_file(const std::filesystem::path& src);

    // Import in-memory bytes as a new large object, the body supplied directly
    // (no source file). Identical to import_file in every other respect — assigns
    // a UUID id + `name`, fingerprints the body with SHA-256, writes blob +
    // meta.json, and captures the metadata into the CRDT — so the body still
    // transfers peer-to-peer and is verified against size + sha256. Used by the
    // partitioned write path (#511) to store a partition's serialized rows as a
    // blob. Returns nullopt only if the body can't be written to disk.
    std::optional<LargeObject> import_bytes(std::string name,
                                            const std::string& bytes);

    // Register metadata for a replicated object (from a peer), without capturing
    // it again. The body may not be here yet. Returns the registered object.
    LargeObject materialize_object(LargeObject object);
    // Remove a replicated object locally (a peer unlinked it), without capturing.
    bool materialize_unlink(const std::string& id);

    // Write a stored object's bytes to `dst` (lo_export). False if id unknown,
    // the body isn't present, or the write fails.
    bool export_file(const std::string& id,
                     const std::filesystem::path& dst) const;

    // Read a stored object's raw bytes (lo_read). Nullopt if id unknown or the
    // body isn't present locally yet.
    std::optional<std::string> read(const std::string& id) const;

    // Whether this object's body is on disk here.
    bool has_body(const std::string& id) const;

    // The on-disk path of an object's body, whether or not it is present yet.
    // A body is content-addressed and written once, so the path is stable and
    // the bytes behind it never change — which is what lets a **non-owner
    // thread** read a body it was handed the path to (the scatter-gather SELECT
    // reads partition bodies on the cluster's network thread, where the
    // in-memory object index is off limits). Does NOT consult that index.
    std::filesystem::path body_path(const std::string& id) const;

    // Delete a stored object (lo_unlink) + remove it from the CRDT. True if one
    // was removed.
    bool unlink(const std::string& id);

    // All objects, sorted by name.
    std::vector<LargeObject> list() const;
    std::size_t count() const { return objects_.size(); }

private:
    void load();
    const LargeObject* find(const std::string& id) const;
    std::filesystem::path object_dir(const std::string& id) const;
    void write_meta(const LargeObject& object) const;
    void sort_objects();

    std::filesystem::path dir_;
    std::string database_id_;          // for the CRDT `lo:` value's `db` field
    zetta::cluster::ReplicationManager* replication_;  // null = not replicated
    std::vector<LargeObject> objects_;
};

}  // namespace zetta::data