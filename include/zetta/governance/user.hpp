#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::governance {

// Whether a user may authenticate. A disabled user's tokens stop validating
// without deleting the user or its history (the reversible counterpart to
// dropping it). The canonical lowercase name is BOTH the display label AND the
// stored representation (CRDT value + user.json), so it is a stable identifier.
enum class UserStatus { Active, Disabled };

std::string_view to_string(UserStatus status);
std::optional<UserStatus> user_status_from(std::string_view name);

// The User JSON Schema (draft 2020-12) — the code-owned source of truth for the
// user.json / replicated `user:<uuid>` value shape; schemas/user.schema.json is
// generated from it and drift-checked by tests/test_schemas.cpp.
const nlohmann::json& user_schema();

// A User (FEAT-9 / #744): the authenticatable identity the token + role system
// hangs off — conceptually a Principal of kind User in the new access model
// (docs/governance/access-control.md). It carries a display name, a status, and
// a created stamp; its rights come from the roles assigned to it (UserRole) and
// a bearer authenticates AS a user by presenting one of its Tokens.
//
// A User is **replicated state** (the same capture/reconcile path as Principal):
// the Automerge CRDT document is the source of truth (flat key `user:<uuid>`),
// and this is the in-memory + on-disk projection — persisted in its own
// UUID-named folder under the data dir with user.json holding the metadata. The
// id is the folder name and never changes; a status/name change rewrites
// user.json but keeps the folder/id.
class User {
public:
    User(std::string id, std::string name, UserStatus status,
         std::string created, std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    UserStatus status() const { return status_; }
    bool active() const { return status_ == UserStatus::Active; }
    const std::string& created() const { return created_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Replace the mutable metadata to match a (replicated) value, then rewrite
    // user.json. The folder/id is unchanged; `created` is logically immutable
    // but accepted so the projection matches the document exactly.
    void set_metadata(std::string name, UserStatus status, std::string created);

    // The replicated/on-disk value shape: { name, status, created }. (The id is
    // the CRDT key / folder name, so it isn't repeated here.)
    nlohmann::json metadata() const;

    // Write this user's metadata (id + fields) to dir/user.json.
    void persist_meta() const;

private:
    std::string id_;
    std::string name_;
    UserStatus status_;
    std::string created_;
    std::filesystem::path dir_;
};

}  // namespace zetta::governance
