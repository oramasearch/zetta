#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/governance/capability.hpp"

namespace zetta::governance {

// The RBAC permission vocabulary (FEAT-9 / #746) — the single, referenceable set
// of product-level permission atoms a role groups. A **permission** is a stable
// `<domain>:<action>` name (e.g. "sql:read", "roles:manage") that maps onto one
// or more concrete Capability{domain,action} the enforcement layer checks. Roles
// (built-in and custom) are named groupings of these; a user's effective rights
// are the union of the operations its roles' permissions expand to.
//
// This is the layer over the raw CapabilityDomain enum (capability.hpp, three
// coarse governance domains): the vocabulary names are what a `GRANT <perm> TO
// ROLE` statement accepts and what the system.roles view renders, while the
// operations they expand to are what Node::authorize_user / Node::authorize
// decide against. Keeping the mapping in one table means the grammar, the
// built-in seed, and the read view can never disagree.
//
// The canonical atoms (issue #746):
//   sql:read       — read rows via SQL / the query engine
//   sql:write      — insert/update/delete rows via SQL
//   sql:ddl        — create/drop databases + tables (schema changes)
//   mcp:call       — invoke the MCP/REST capability surface (the API call right)
//   blob:read      — read large-object (blob) bodies (object-scopable by prefix, #747)
//   serving:read   — call the governed serving routes (predict + search)
//   users:manage   — create/disable/drop users
//   roles:manage   — create/edit/drop roles and assign them to users
//   tokens:manage  — issue/revoke bearer tokens
//   cluster:admin  — cluster-wide administration
//
// The mapping onto Capability{domain,action} (the enforcement atoms actually
// checked today; sql:*/serving:* names reserve the finer data rights that the
// scoped-grants / RLS slices — epic #743 items 4/5 — will enforce per-statement):
//   sql:read       -> data:read
//   sql:write      -> data:write
//   sql:ddl        -> data:ddl
//   mcp:call       -> api:query
//   blob:read      -> data:blob_read
//   serving:read   -> data:predict, data:search
//   users:manage   -> api:manage_users
//   roles:manage   -> api:manage_roles
//   tokens:manage  -> api:manage_tokens
//   cluster:admin  -> api:cluster_admin

// The canonical permission names, in a stable vocabulary order. This is THE list
// custom roles pick from and the order the system.roles view renders in.
const std::vector<std::string>& permission_vocabulary();

// True iff `name` is a permission in the vocabulary (the boundary check a
// GRANT/REVOKE statement runs before touching a role).
bool is_permission(std::string_view name);

// The operations a permission expands to (the deny-by-default rights it grants),
// or an empty vector for an unknown name. A permission may expand to more than
// one operation (serving:read covers both predict and search).
std::vector<Operation> permission_operations(std::string_view name);

// Expand permission NAMES into a CapabilitySet — the de-duplicated union of every
// named permission's operations (no TTL). Unknown names contribute nothing;
// validate with is_permission first for a hard error at the grammar boundary.
CapabilitySet capabilities_for_permissions(const std::vector<std::string>& names);

// The reverse view: the permission names a CapabilitySet fully satisfies, in
// vocabulary order. A permission is "present" iff EVERY operation it expands to
// is granted (unexpired as of `now`). Used to render a role's permission set in
// system.roles. `now` bounds TTL'd capabilities (pass the current time).
std::vector<std::string> permissions_of(const CapabilitySet& capabilities,
                                        std::string_view now);

// The permission-name set for a built-in role ("admin"/"writer"/"reader"), or
// std::nullopt for an unknown name. The built-ins are defined purely in terms of
// the vocabulary — builtin_role_permissions() (role.hpp) expands these:
//   admin  — every permission (incl. *:manage + cluster:admin) — unrestricted
//   writer — sql:read, sql:write, sql:ddl, mcp:call, serving:read
//   reader — sql:read, serving:read, mcp:call
std::optional<std::vector<std::string>> builtin_role_permission_names(
    std::string_view role);

}  // namespace zetta::governance
