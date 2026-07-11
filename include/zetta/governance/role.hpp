#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/governance/capability.hpp"
#include "zetta/governance/grant.hpp"
#include "zetta/governance/scope.hpp"

namespace zetta::governance {

// A named grouping of permissions along the axes an AccessKey carries — a Scope
// (which resources) + a CapabilitySet (which rights) — PLUS a list of
// object-scoped grants (#747). A role is the unit a user is granted (via
// UserRole); a user's effective reach is the union of its roles' permissions.
//
// `capabilities` are the flat, UNSCOPED rights (granted on every object — the
// coarse behavior the built-ins use); `scoped_grants` are the fine-grained rights
// bound to specific objects (`GRANT <perm> ON <object> TO ROLE`). The two
// compose: a flat right covers everything, a scoped right only its object.
struct RolePermissions {
    Scope scope;
    CapabilitySet capabilities;
    std::vector<ScopedGrant> scoped_grants;  // object-scoped rights (#747)

    bool operator==(const RolePermissions&) const = default;
};

// The permissions for a built-in role name ("admin" / "writer" / "reader"), or
// std::nullopt for an unknown name. Defined in terms of the permission
// vocabulary (permission.hpp / #746) — see builtin_role_permission_names() — and
// expanded to a CapabilitySet here. The three built-ins are unrestricted in
// scope (they reach every resource) and differ only in rights:
//   admin  — every permission (incl. users:manage/roles:manage/tokens:manage/
//            cluster:admin)
//   writer — sql:read, sql:write, sql:ddl, mcp:call, serving:read
//   reader — sql:read, serving:read, mcp:call
// Deny-by-default still applies: a right not listed is denied.
std::optional<RolePermissions> builtin_role_permissions(std::string_view name);

// The names of the built-in roles, in privilege order (admin, writer, reader).
// Used by the bootstrap seeder and to seed the role store on first use.
const std::vector<std::string>& builtin_role_names();

// The Role JSON Schema (draft 2020-12) — the code-owned source of truth for the
// role.json / replicated `role:<uuid>` value shape; schemas/role.schema.json is
// generated from it and drift-checked by tests/test_schemas.cpp. The scope +
// capability sub-shapes mirror access_key.schema.json's $defs.
const nlohmann::json& role_schema();

// A Role (FEAT-9 / #744): a named permission set, replicated as `role:<uuid>`.
// **Replicated state** (the Principal capture/reconcile path): the CRDT document
// is the source of truth, and this is its in-memory + on-disk projection —
// persisted in a UUID-named folder under the data dir with role.json holding the
// metadata. The id is the folder name and never changes.
class Role {
public:
    Role(std::string id, std::string name, RolePermissions permissions,
         std::string created, std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const RolePermissions& permissions() const { return permissions_; }
    const Scope& scope() const { return permissions_.scope; }
    const CapabilitySet& capabilities() const { return permissions_.capabilities; }
    const std::string& created() const { return created_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Replace the mutable metadata to match a (replicated) value, then rewrite
    // role.json. The folder/id is unchanged.
    void set_metadata(std::string name, RolePermissions permissions,
                      std::string created);

    // The replicated/on-disk value shape: { name, scope, capabilities, created }.
    nlohmann::json metadata() const;

    // Write this role's metadata (id + fields) to dir/role.json.
    void persist_meta() const;

private:
    std::string id_;
    std::string name_;
    RolePermissions permissions_;
    std::string created_;
    std::filesystem::path dir_;
};

// Parse a role's { scope, capabilities } permissions from JSON at the trust
// boundary (mirrors scope_from_json / capability_set_from_json). std::nullopt on
// anything malformed so a corrupt role.json is skipped rather than granting the
// wrong reach.
std::optional<RolePermissions> role_permissions_from_json(
    const nlohmann::json& document);

}  // namespace zetta::governance
