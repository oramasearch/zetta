#include "zetta/query/service.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "zetta/query/ast.hpp"

namespace zetta::query {

namespace {

// The statement's kind as SQL names it, for a read_only refusal.
std::string_view statement_kind(const Statement& statement) {
    return std::visit(
        [](const auto& node_statement) -> std::string_view {
            using Kind = std::decay_t<decltype(node_statement)>;
            if constexpr (std::is_same_v<Kind, CreateDatabaseStmt>) return "CREATE DATABASE";
            else if constexpr (std::is_same_v<Kind, DropDatabaseStmt>) return "DROP DATABASE";
            else if constexpr (std::is_same_v<Kind, CreateUserStmt>) return "CREATE USER";
            else if constexpr (std::is_same_v<Kind, CreateTokenStmt>) return "CREATE TOKEN";
            else if constexpr (std::is_same_v<Kind, CreateRoleStmt>) return "CREATE ROLE";
            else if constexpr (std::is_same_v<Kind, DropRoleStmt>) return "DROP ROLE";
            else if constexpr (std::is_same_v<Kind, GrantPermissionStmt> ||
                               std::is_same_v<Kind, GrantRoleStmt>) return "GRANT";
            else if constexpr (std::is_same_v<Kind, RevokePermissionStmt> ||
                               std::is_same_v<Kind, RevokeRoleStmt>) return "REVOKE";
            else if constexpr (std::is_same_v<Kind, CreatePolicyStmt>) return "CREATE POLICY";
            else if constexpr (std::is_same_v<Kind, DropPolicyStmt>) return "DROP POLICY";
            else if constexpr (std::is_same_v<Kind, CreateTableStmt>) return "CREATE TABLE";
            else if constexpr (std::is_same_v<Kind, DropTableStmt>) return "DROP TABLE";
            else if constexpr (std::is_same_v<Kind, InsertStmt>) return "INSERT";
            else if constexpr (std::is_same_v<Kind, CreateVirtualTableStmt>) return "CREATE VIRTUAL TABLE";
            else if constexpr (std::is_same_v<Kind, UpdateStmt>) return "UPDATE";
            else if constexpr (std::is_same_v<Kind, DeleteStmt>) return "DELETE";
            else if constexpr (std::is_same_v<Kind, TruncateStmt>) return "TRUNCATE";
            else if constexpr (std::is_same_v<Kind, SelectStmt>) return "SELECT";
            else return "statement";
        },
        statement);
}

}  // namespace

QueryService::Outcome QueryService::run(std::string_view sql,
                                        const std::string& current_database,
                                        const RunOptions& options) {
    Outcome out;

    // Time the whole parse + execute; the stopwatch writes elapsed on every
    // return path (including early errors).
    struct Stopwatch {
        std::chrono::steady_clock::time_point started;
        std::chrono::nanoseconds& elapsed;
        ~Stopwatch() {
            elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started);
        }
    } stopwatch{std::chrono::steady_clock::now(), out.elapsed};

    // This boundary is non-throwing (the zetta-core boundary rule). The parser never
    // throws: it returns every error as a value (#793, #819). The executor may still
    // throw on something it does not handle, so contain any throw as a normal query
    // error rather than let one statement end the node.
    try {
        const ParseResult parsed = dialect_.parse(sql);
        if (!parsed.ok()) {
            out.error = parsed.error;
            return out;
        }
        // read_only (#1501): decided on the engine's own parse, before any
        // statement runs, so no text can be one read to a caller's guard and a
        // write here. Exactly one statement, and it must be a SELECT.
        if (options.read_only) {
            if (parsed.statements.size() != 1) {
                out.refused = true;
                out.error = "read_only: expected exactly one read statement, got " +
                            std::to_string(parsed.statements.size());
                return out;
            }
            if (!std::holds_alternative<SelectStmt>(parsed.statements.front())) {
                out.refused = true;
                out.error = std::string("read_only: refused a ") +
                            std::string(statement_kind(parsed.statements.front())) +
                            " statement; only a SELECT runs";
                return out;
            }
        }
        for (const Statement& stmt : parsed.statements) {
            QueryResult r = executor_.execute(stmt, current_database);
            if (!r.ok()) {
                out.error = r.error;
                return out;  // stop at the first failing statement
            }
            out.messages.push_back(std::move(r.message));
            // Surface the result set for a SELECT, and for any other statement that
            // returns rows (e.g. CREATE TOKEN returns the plaintext secret once).
            if (std::holds_alternative<SelectStmt>(stmt) || !r.rows.empty()) {
                out.columns = std::move(r.columns);
                out.rows = std::move(r.rows);
            }
        }
    } catch (const std::exception& error) {
        out.error = std::string("query not supported: ") + error.what();
        out.columns.clear();
        out.rows.clear();
    } catch (...) {
        out.error = "query not supported";
        out.columns.clear();
        out.rows.clear();
    }
    return out;
}

}  // namespace zetta::query
