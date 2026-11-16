// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "zetta/query/ast.hpp"

namespace zetta::query {

// Result of parsing dialect text into generic statements.
struct ParseResult {
    std::vector<Statement> statements;
    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// A SQL dialect frontend: turns dialect-specific text into the generic,
// dialect-neutral statement AST (ast.hpp) that the executor runs. Add support
// for another database format by implementing this interface — the executor
// and the rest of the pipeline are dialect-agnostic and reused as-is.
class Dialect {
public:
    virtual ~Dialect() = default;

    // Short dialect name, e.g. "postgres".
    virtual const char* name() const = 0;

    // Parse `sql` (which may contain several ';'-separated statements) into the
    // generic AST. On failure, ParseResult::error explains why.
    virtual ParseResult parse(std::string_view sql) const = 0;
};

}  // namespace zetta::query
