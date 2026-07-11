#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/governance/capability.hpp"
#include "zetta/query/ast.hpp"

namespace zetta {

class Node;
namespace cluster { class Cluster; }
namespace query {

// Outcome of executing one statement.
struct QueryResult {
    std::string message;               // e.g. "CREATE DATABASE", "INSERT 0 1", "SELECT 3"
    std::vector<std::string> columns;  // projection columns for SELECT (empty for SELECT *)
    std::vector<nlohmann::json> rows;  // result rows (SELECT)
    std::string error;                 // empty on success

    bool ok() const { return error.empty(); }
};

// Every table the virtual `system` database serves, alphabetical (#684). The
// single source for "which system tables exist": the `system.tables` read view
// returns one row per entry and the headed explorer renders this list — the
// GUI never keeps its own copy (RULE-7). See docs/data/query.md.
std::span<const std::string_view> system_table_names();

// Runs generic statements against a Node. `current_database` is the default for
// unqualified table references (the UI's selected database). Dialect-neutral:
// it only ever sees the generic AST.
//
// An optional Cluster lets SELECT read the `nodes` system table (all nodes
// + replication status); without one, querying it returns a clear error.
//
// `acting_key` / `acting_is_user` carry the identity the API gate resolved (#745)
// so per-statement authorization can run inside the engine: role-management DDL
// (CREATE ROLE, GRANT/REVOKE …) is gated on the roles:manage permission (#746).
// The default — an empty acting key — is the unauthenticated-local session (the
// library / GUI / test path), which is trusted and always allowed, exactly as
// the API layer treats an empty acting key.
class Executor {
public:
    explicit Executor(Node& node, const zetta::cluster::Cluster* cluster = nullptr,
                      std::string acting_key = {}, bool acting_is_user = false)
        : node_(node),
          cluster_(cluster),
          acting_key_(std::move(acting_key)),
          acting_is_user_(acting_is_user) {}

    QueryResult execute(const Statement& stmt, const std::string& current_database);

private:
    // Authorize a role-management operation against the acting identity. Returns
    // an empty optional when allowed; a deny reason otherwise. An empty acting key
    // is the trusted local session (allowed); a token-resolved user is checked
    // against its effective permissions; a legacy AccessKey against its own.
    std::optional<std::string> require_permission(
        const zetta::governance::Operation& operation) const;

    Node& node_;
    const zetta::cluster::Cluster* cluster_;
    std::string acting_key_;
    bool acting_is_user_;
};

}  // namespace query
}  // namespace zetta
