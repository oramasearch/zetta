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
    // `SELECT * FROM system.*`). This boundary should be non-throwing (the zetta-core
    // seam rule), so contain any throw as a normal query error.
    // NOTE (#819): a known Clang miscompile of postgres.cpp's exception unwinding at
    // -O1 + -fexperimental-library (see the CMakeLists "Toolchain workaround" note)
    // can std::terminate() the process mid-unwind *before* this catch runs, so this
    // does NOT yet fully contain those specific parses — it is the correct backstop
    // once the compiler is bumped / the throwing helper is made non-throwing.
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
