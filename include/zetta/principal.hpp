#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/access_key.hpp"
#include "zetta/scope.hpp"

namespace zetta {

class ReplicationManager;

// What sort of actor a principal is (ENT-20): an authenticated actor is a user,
// a service, or a node. The kind is governance context (the subject of grants +
// audit); access itself is decided by the key's scope, not the kind.
enum class PrincipalKind { User, Service, Node };

// Canonical lowercase name of a principal kind ("user" / "service" / "node") —
// BOTH the display label AND the stored representation written to the CRDT +
// principal.json, so it is a stable on-disk identifier.
std::string_view to_string(PrincipalKind kind);

// Parse a stored/canonical kind name back to the enum; std::nullopt on an
// unknown spelling — the boundary check for the enum domain.
std::optional<PrincipalKind> principal_kind_from(std::string_view name);

// A Principal (ENT-20): an authenticated actor — user, service, or node — the
// subject of access grants and (later) the audit trail. This realizes the V1
// governance model with AccessKey + Scope: a principal owns its **access keys**,
// and a request inherits the scope of the key it logged in with (RULE-3).
//
// A Principal is **replicated state**: the Automerge CRDT document is the source
// of truth (flat key `principal:<uuid>`), and a Principal is the in-memory +
// on-disk projection of it — persisted in its own folder (named by the UUID)
// under the node's data dir, with principal.json holding the metadata. It owns
// its **keys** the same way a Project owns its inputs: key create/drop are
// captured into the CRDT and `materialize_key` is the reverse path used by
// reconciliation. Keys live in the principal's `keys/` subfolder.
//
// The id is the folder name and never changes; rename rewrites principal.json,
// but keeps the folder/id. `metadata()` is the value shape that rides in the
// CRDT and (with the id added) on disk.
class Principal {
public:
    Principal(std::string id, PrincipalKind kind, std::string name,
              std::string created, std::filesystem::path dir,
              ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    PrincipalKind kind() const { return kind_; }
    const std::string& name() const { return name_; }
    const std::string& created() const { return created_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Replace the mutable metadata to match a (replicated) value, then rewrite
    // principal.json. The folder/id is unchanged. `created` is logically
    // immutable but accepted so the projection matches the document exactly.
    void set_metadata(PrincipalKind kind, std::string name, std::string created);

    // Write this principal's metadata (id + fields) to dir/principal.json.
    void persist_meta() const;

    // The replicated/on-disk value shape: { kind, name, created }. (The id is
    // the CRDT key / folder name, so it isn't repeated here.)
    nlohmann::json metadata() const;

    // (Re)load keys by scanning dir/keys for key folders. Called on node start
    // so a key survives restart (standalone reads it straight off disk).
    void load();

    // Issue a new access key with the given scope: stamps id + created, writes
    // the metadata into the CRDT (`key:<uuid>`) and key.json to disk, and returns
    // the handle. A key is immutable once created. Empty handle on disk failure.
    std::shared_ptr<AccessKey> create_key(Scope scope);
    // Apply a replicated key (from a peer) by id, without capturing. Returns the
    // existing handle if already present.
    std::shared_ptr<AccessKey> materialize_key(std::string id, Scope scope,
                                               std::string created);
    // Find a key by its id (UUID), or an empty handle.
    std::shared_ptr<AccessKey> find_key(const std::string& id) const;
    // Drop a key (removes its folder + the CRDT key); true if one was removed.
    bool drop_key(const std::string& id);
    // Drop a key by id without capturing (reconciliation removing a peer's drop).
    bool materialize_drop_key(const std::string& id);

    const std::vector<std::shared_ptr<AccessKey>>& keys() const { return keys_; }
    std::size_t key_count() const { return keys_.size(); }

private:
    std::filesystem::path keys_dir() const { return dir_ / "keys"; }

    std::string id_;
    PrincipalKind kind_;
    std::string name_;
    std::string created_;
    std::filesystem::path dir_;
    std::vector<std::shared_ptr<AccessKey>> keys_;
    ReplicationManager* replication_;  // null = not replicated
};

}  // namespace zetta
