#pragma once

#include <optional>

#include <nlohmann/json.hpp>

#include "zetta/query/ast.hpp"

// The WHERE comparison primitive, shared by everything that filters rows.
//
// It lives here rather than inside the executor's translation unit because a
// `SELECT` over a [partition-only table](../../../docs/data/partition-only-tables.md)
// is filtered **on the partition's owner** (compute-near-data), not on the node
// that parsed the SQL — and two implementations of "does this row match" would
// be two chances for a scattered query to disagree with a local one.
//
// Nothing here throws.
namespace zetta::query {

// Whether `cell` <op> `target` holds. Equality/inequality work over any JSON
// value (nlohmann compares numbers across int/float); the ordering comparisons
// apply numerically when both sides are numbers and lexicographically when both
// are strings, and otherwise do not match (mixed/unsupported types).
bool compare_cell(const nlohmann::json& cell, CompareOp op,
                  const nlohmann::json& target);

// Whether a row satisfies an optional WHERE clause. A row lacking the column
// never matches (it is excluded), preserving the original equality behavior.
bool row_matches(const nlohmann::json& row,
                 const std::optional<WhereClause>& where);

}  // namespace zetta::query
