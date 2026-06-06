#include "zetta/query/service.hpp"

#include <utility>
#include <variant>

#include "zetta/query/ast.hpp"

namespace zetta::query {

QueryService::Outcome QueryService::run(std::string_view sql,
                                        const std::string& current_database) {
    Outcome out;
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
        if (std::holds_alternative<SelectStmt>(stmt)) {
            out.columns = std::move(r.columns);
            out.rows = std::move(r.rows);
        }
    }
    return out;
}

}  // namespace zetta::query
