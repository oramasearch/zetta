#include "zetta/query/service.hpp"

#include <chrono>
#include <exception>
#include <utility>
#include <variant>

#include "zetta/query/ast.hpp"

namespace zetta::query {

QueryService::Outcome QueryService::run(std::string_view sql,
                                        const std::string& current_database) {
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

    // The parser/executor still THROW on some unsupported syntax (e.g. DISTINCT,
    // `SELECT * FROM system.*`). An uncaught throw here aborts the whole node — a DoS
    // trivially reachable from the web shell (#819). Contain any throw as a normal
    // query error so this boundary stays non-throwing (the zetta-core seam rule).
    try {
        const ParseResult parsed = dialect_.parse(sql);
        if (!parsed.ok()) {
            out.error = parsed.error;
            return out;
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
