#include <utility>
#include <vector>

#include "zetta/query/ast.hpp"

// Statement builders that must NOT live in the SQL parser's translation unit.
//
// Constructing these variant alternatives inside src/core/query/postgres.cpp
// miscompiles that TU's exception unwinding under the pinned Clang +
// -fexperimental-library: a thrown ParseError escapes the parser's try/catch
// (and even an outer catch) and std::terminate()s the process on any malformed
// statement. Isolating the construction here — a TU with no exception machinery —
// lets the parser stay correct: it only ever *moves* the returned Statement,
// never converting-constructs it. The role-management statements (#746) carry
// multiple std::string / vector<string> members, exactly the throwing-temporary
// shape that triggered the miscompile for CREATE TOKEN, so they route through
// here too. This is a narrow toolchain workaround; drop it when the compiler is
// upgraded.
namespace zetta::query {

Statement make_create_token_statement(std::string user, std::string type) {
    return CreateTokenStmt{std::move(user), std::move(type)};
}

Statement make_create_role_statement(std::string name) {
    return CreateRoleStmt{std::move(name)};
}

Statement make_drop_role_statement(std::string name) {
    return DropRoleStmt{std::move(name)};
}

Statement make_grant_permission_statement(std::string role,
                                          std::vector<std::string> permissions,
                                          GrantObject object) {
    return GrantPermissionStmt{std::move(role), std::move(permissions),
                               std::move(object)};
}

Statement make_revoke_permission_statement(
    std::string role, std::vector<std::string> permissions,
    GrantObject object) {
    return RevokePermissionStmt{std::move(role), std::move(permissions),
                                std::move(object)};
}

Statement make_grant_role_statement(std::string role, std::string user) {
    return GrantRoleStmt{std::move(role), std::move(user)};
}

Statement make_revoke_role_statement(std::string role, std::string user) {
    return RevokeRoleStmt{std::move(role), std::move(user)};
}

}  // namespace zetta::query
