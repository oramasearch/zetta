// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/governance/capability.hpp"
#include "zetta/governance/scope.hpp"

namespace zetta::governance {

// A **scoped grant** (#747, epic #743 item 4): a specific right (an Operation)
// bound to a specific object (a Resource). It is the fine-grained unit that
// NARROWS a permission to an object — the `GRANT <perm> ON <object> TO ROLE`
// form. A role carries a flat CapabilitySet (the coarse, unscoped rights it
// grants everywhere) PLUS a list of these (the rights it grants only on named
// objects). A user's effective scoped grants are the union across its roles.
//
// The distinction the enforcement layer draws:
//   - a flat capability (`GRANT sql:read TO ROLE`) grants the operation on EVERY
//     object — the pre-#747 behavior, unchanged (the built-ins work this way);
//   - a scoped grant (`GRANT sql:read ON db.t(a,b) TO ROLE`) grants the
//     operation ONLY on objects its Resource covers (object_covers, scope.hpp).
struct ScopedGrant {
    Operation operation;  // the right (e.g. {Data, "read"} for sql:read)
    Resource object;      // the object it applies to (table/column/blob/tool/db)

    bool operator==(const ScopedGrant&) const = default;
};

// JSON serialization (ADL hook): { "operation": { "domain", "action" },
// "object": <Resource> }. The operation reuses the capability domain spelling.
void to_json(nlohmann::json& document, const ScopedGrant& grant);

// Parse a scoped grant at the trust boundary; std::nullopt on anything malformed
// (not an object, missing/mis-typed fields, an unknown domain or resource kind)
// so a corrupt role.json entry is skipped rather than granting the wrong reach.
std::optional<ScopedGrant> scoped_grant_from_json(const nlohmann::json& document);

// The column-level access an identity has to one table for one operation
// (read / write). The result of folding the identity's grants (grant.cpp):
//   - table_allowed=false — the identity may not touch the table at all;
//   - all_columns=true    — no column restriction (a global, database, or
//                           whole-table grant, or an admin/unrestricted bypass);
//   - otherwise           — `columns` are the ONLY columns allowed; the rest are
//                           masked (read) or rejected (write).
struct ColumnAccess {
    bool table_allowed = false;
    bool all_columns = false;
    std::vector<std::string> columns;

    // Is `column` accessible? Deny if the table is off-limits; allow when there
    // is no column restriction; otherwise only the listed columns.
    bool allows(const std::string& column) const;
};

// Fold an identity's rights into the column access for `db.table` under
// `operation` (#747). `global` is true when the identity holds `operation`
// UNSCOPED (a flat capability) or bypasses object scoping (admin/unrestricted)
// — then every column is allowed. Otherwise the decision is driven by the scoped
// grants: a database- or whole-table grant opens every column; column-set grants
// contribute their columns; no covering grant leaves the table off-limits.
ColumnAccess column_access_for(bool global,
                               const std::vector<ScopedGrant>& grants,
                               const Operation& operation,
                               const std::string& database,
                               const std::string& table);

// Does the identity's rights admit `operation` on `object`? `global` is the same
// unscoped/bypass flag as above (true => allowed on everything); otherwise true
// iff some scoped grant for `operation` covers `object` (object_covers). Used for
// the blob and MCP-tool decisions (and any single-object check).
bool object_permitted(bool global, const std::vector<ScopedGrant>& grants,
                      const Operation& operation, const Resource& object);

}  // namespace zetta::governance
