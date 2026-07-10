#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace zetta::governance {

// The User JSON Schema for a user↔role assignment (draft 2020-12).
const nlohmann::json& user_role_schema();

// A UserRole (FEAT-9 / #744): the many-to-many assignment tying a User to a Role
// — a user's effective permissions are the union of the roles assigned to it.
// **Replicated state** (the Principal capture/reconcile path): the CRDT document
// is the source of truth (`user_role:<uuid>`), and this is its in-memory +
// on-disk projection under a UUID-named folder with user_role.json. The id is the
// folder name and never changes; an assignment is immutable (to change it, drop
// and re-create).
class UserRole {
public:
    UserRole(std::string id, std::string user_id, std::string role_id,
             std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& user_id() const { return user_id_; }
    const std::string& role_id() const { return role_id_; }
    const std::filesystem::path& dir() const { return dir_; }

    // The replicated/on-disk value shape: { user, role } (the id is the CRDT key
    // / folder name).
    nlohmann::json metadata() const;

    // Write this assignment's metadata (id + fields) to dir/user_role.json.
    void persist_meta() const;

private:
    std::string id_;
    std::string user_id_;
    std::string role_id_;
    std::filesystem::path dir_;
};

}  // namespace zetta::governance
