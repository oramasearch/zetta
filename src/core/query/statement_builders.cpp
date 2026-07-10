#include <utility>

#include "zetta/query/ast.hpp"

// Statement builders that must NOT live in the SQL parser's translation unit.
//
// Constructing the CreateTokenStmt variant alternative inside
// src/core/query/postgres.cpp miscompiles that TU's exception unwinding under
// the pinned Clang + -fexperimental-library: a thrown ParseError escapes the
// parser's try/catch (and even an outer catch) and std::terminate()s the
// process on any malformed statement. Isolating the construction here — a TU
// with no exception machinery — lets the parser stay correct: it only ever
// *moves* the returned Statement, never converting-constructs it. This is a
// narrow toolchain workaround; drop it when the compiler is upgraded.
namespace zetta::query {

Statement make_create_token_statement(std::string user, std::string type) {
    return CreateTokenStmt{std::move(user), std::move(type)};
}

}  // namespace zetta::query
