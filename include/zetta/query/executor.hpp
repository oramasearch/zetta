#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/query/ast.hpp"

namespace zetta {

class Node;
class Cluster;

namespace query {

// Outcome of executing one statement.
struct QueryResult {
    std::string message;               // e.g. "CREATE DATABASE", "INSERT 0 1", "SELECT 3"
    std::vector<std::string> columns;  // projection columns for SELECT (empty for SELECT *)
    std::vector<nlohmann::json> rows;  // result rows (SELECT)
    std::string error;                 // empty on success

    bool ok() const { return error.empty(); }
};

// Runs generic statements against a Node. `current_database` is the default for
// unqualified table references (the UI's selected database). Dialect-neutral:
// it only ever sees the generic AST.
//
// An optional Cluster lets SELECT read the `zetta_nodes` system table (all nodes
// + replication status); without one, querying it returns a clear error.
class Executor {
public:
    explicit Executor(Node& node, const Cluster* cluster = nullptr)
        : node_(node), cluster_(cluster) {}

    QueryResult execute(const Statement& stmt, const std::string& current_database);

private:
    Node& node_;
    const Cluster* cluster_;
};

}  // namespace query
}  // namespace zetta
