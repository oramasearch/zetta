#include "zetta/query/postgres.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::query {
namespace {

// Thrown internally on a parse failure and caught at the dialect boundary —
// exceptions never escape PostgresDialect::parse().
struct ParseError {
    std::string message;
};

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

enum class Tok { Ident, Num, Str, Punct, End };

struct Token {
    Tok type;
    std::string text;  // identifier/number text, string contents, or the punct char
};

class Parser {
public:
    explicit Parser(std::string_view sql) : src_(sql) {}

    ParseResult run() {
        ParseResult result;
        try {
            tokenize();
            while (!at_end()) {
                if (accept_punct(';')) continue;  // stray/empty statement
                result.statements.push_back(parse_statement());
                if (!at_end() && !accept_punct(';')) {
                    throw ParseError{"expected ';' after statement, got '" +
                                     peek().text + "'"};
                }
            }
        } catch (const ParseError& e) {
            result.statements.clear();
            result.error = e.message;
        }
        return result;
    }

private:
    // --- tokenizer ----------------------------------------------------------
    void tokenize() {
        const std::string s(src_);
        const std::size_t n = s.size();
        const auto ident_start = [](char c) {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
        };
        const auto ident_char = [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        };
        std::size_t i = 0;
        while (i < n) {
            const char c = s[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++i;
                continue;
            }
            if (c == '-' && i + 1 < n && s[i + 1] == '-') {  // -- line comment
                i += 2;
                while (i < n && s[i] != '\n') ++i;
                continue;
            }
            if (ident_start(c)) {
                std::size_t j = i + 1;
                while (j < n && ident_char(s[j])) ++j;
                tokens_.push_back({Tok::Ident, s.substr(i, j - i)});
                i = j;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                std::size_t j = i + 1;
                while (j < n && (std::isdigit(static_cast<unsigned char>(s[j])) ||
                                 s[j] == '.')) {
                    ++j;
                }
                tokens_.push_back({Tok::Num, s.substr(i, j - i)});
                i = j;
                continue;
            }
            if (c == '\'') {  // single-quoted string, '' escapes a quote
                std::string lit;
                std::size_t j = i + 1;
                bool closed = false;
                while (j < n) {
                    if (s[j] == '\'') {
                        if (j + 1 < n && s[j + 1] == '\'') {
                            lit.push_back('\'');
                            j += 2;
                            continue;
                        }
                        closed = true;
                        ++j;
                        break;
                    }
                    lit.push_back(s[j]);
                    ++j;
                }
                if (!closed) throw ParseError{"unterminated string literal"};
                tokens_.push_back({Tok::Str, lit});
                i = j;
                continue;
            }
            static constexpr std::string_view kPunct = "(),;*.=-<>!:";
            if (kPunct.find(c) != std::string_view::npos) {
                tokens_.push_back({Tok::Punct, std::string(1, c)});
                ++i;
                continue;
            }
            throw ParseError{std::string("unexpected character '") + c + "'"};
        }
        tokens_.push_back({Tok::End, ""});
    }

    // --- cursor -------------------------------------------------------------
    const Token& peek() const { return tokens_[pos_]; }
    const Token& advance() { return tokens_[pos_++]; }
    bool at_end() const { return peek().type == Tok::End; }

    bool accept_keyword(std::string_view kw) {
        if (peek().type == Tok::Ident && iequals(peek().text, kw)) {
            advance();
            return true;
        }
        return false;
    }
    void expect_keyword(std::string_view kw) {
        if (!accept_keyword(kw)) {
            throw ParseError{"expected '" + std::string(kw) + "', got '" +
                             peek().text + "'"};
        }
    }
    bool accept_punct(char c) {
        if (peek().type == Tok::Punct && peek().text.size() == 1 &&
            peek().text[0] == c) {
            advance();
            return true;
        }
        return false;
    }
    void expect_punct(char c) {
        if (!accept_punct(c)) {
            throw ParseError{std::string("expected '") + c + "', got '" +
                             peek().text + "'"};
        }
    }
    std::string parse_ident() {
        if (peek().type != Tok::Ident) {
            throw ParseError{"expected an identifier, got '" + peek().text + "'"};
        }
        return advance().text;
    }

    TableRef parse_table_ref() {
        TableRef ref;
        const std::string first = parse_ident();
        if (accept_punct('.')) {
            ref.database = first;
            ref.table = parse_ident();
        } else {
            ref.table = first;
        }
        return ref;
    }

    // A comparison operator from one or two punctuation tokens: `=`, `!=`, `<>`,
    // `<`, `<=`, `>`, `>=` (the tokenizer emits each char separately, so `<=` is
    // two tokens).
    CompareOp parse_comparison() {
        if (accept_punct('=')) return CompareOp::Equal;
        if (accept_punct('!')) {
            expect_punct('=');
            return CompareOp::NotEqual;
        }
        if (accept_punct('<')) {
            if (accept_punct('=')) return CompareOp::LessEqual;
            if (accept_punct('>')) return CompareOp::NotEqual;
            return CompareOp::Less;
        }
        if (accept_punct('>')) {
            if (accept_punct('=')) return CompareOp::GreaterEqual;
            return CompareOp::Greater;
        }
        throw ParseError{"expected a comparison operator (=, !=, <>, <, <=, >, "
                         ">=), got '" + peek().text + "'"};
    }

    // The aggregate function an identifier names, or nullopt if it is not one.
    static std::optional<AggregateFunc> aggregate_func_of(std::string_view name) {
        if (iequals(name, "count")) return AggregateFunc::Count;
        if (iequals(name, "sum")) return AggregateFunc::Sum;
        if (iequals(name, "min")) return AggregateFunc::Min;
        if (iequals(name, "max")) return AggregateFunc::Max;
        if (iequals(name, "avg") || iequals(name, "mean")) {
            return AggregateFunc::Avg;
        }
        return std::nullopt;
    }

    // Whether the cursor is at an aggregate call: an aggregate-function identifier
    // immediately followed by '(' (so a column literally named `sum` still parses
    // as a column unless it is called).
    bool at_aggregate_call() const {
        return peek().type == Tok::Ident &&
               aggregate_func_of(peek().text).has_value() &&
               pos_ + 1 < tokens_.size() &&
               tokens_[pos_ + 1].type == Tok::Punct &&
               tokens_[pos_ + 1].text == "(";
    }

    nlohmann::json parse_value() {
        const bool negative = accept_punct('-');
        const Token& t = peek();
        if (t.type == Tok::Num) {
            const std::string num = (negative ? "-" : "") + t.text;
            auto v = nlohmann::json::parse(num, nullptr, /*allow_exceptions=*/false);
            if (v.is_discarded() || !v.is_number()) {
                throw ParseError{"invalid number: " + num};
            }
            advance();
            return v;
        }
        if (negative) throw ParseError{"expected a number after '-'"};
        if (t.type == Tok::Str) {
            advance();
            return t.text;
        }
        if (t.type == Tok::Ident) {
            if (iequals(t.text, "true")) { advance(); return true; }
            if (iequals(t.text, "false")) { advance(); return false; }
            if (iequals(t.text, "null")) { advance(); return nullptr; }
        }
        throw ParseError{"expected a value, got '" + t.text + "'"};
    }

    // --- statements ---------------------------------------------------------
    Statement parse_statement() {
        if (accept_keyword("create")) return parse_create();
        if (accept_keyword("drop")) return parse_drop();
        if (accept_keyword("insert")) return parse_insert();
        if (accept_keyword("select")) return parse_select();
        if (accept_keyword("grant")) return parse_grant();
        if (accept_keyword("revoke")) return parse_revoke();
        throw ParseError{"unsupported statement, expected CREATE/DROP/INSERT/"
                         "SELECT/GRANT/REVOKE, got '" + peek().text + "'"};
    }

    Statement parse_create() {
        if (accept_keyword("database")) {
            return CreateDatabaseStmt{parse_ident()};
        }
        if (accept_keyword("table")) {
            CreateTableStmt stmt;
            stmt.table = parse_table_ref();
            expect_punct('(');
            if (!accept_punct(')')) {
                do {
                    ColumnDef col;
                    col.name = parse_ident();
                    if (peek().type == Tok::Ident) {
                        col.type = parse_ident();
                        skip_type_modifier();
                    }
                    stmt.columns.push_back(std::move(col));
                } while (accept_punct(','));
                expect_punct(')');
            }
            return stmt;
        }
        if (accept_keyword("user")) {
            return CreateUserStmt{parse_ident()};
        }
        if (accept_keyword("token")) {
            return parse_create_token();
        }
        if (accept_keyword("role")) {
            return parse_create_role();
        }
        if (accept_keyword("policy")) {
            return parse_create_policy();
        }
        throw ParseError{"expected DATABASE, TABLE, USER, TOKEN, ROLE, or POLICY "
                         "after CREATE"};
    }

    // CREATE ROLE <name>. Out-of-line + variant built in a separate TU — the same
    // toolchain workaround as CREATE TOKEN (see parse_create_token / ast.hpp).
    [[gnu::noinline]] Statement parse_create_role() {
        return make_create_role_statement(parse_ident());
    }

    // CREATE TOKEN FOR <user> [TYPE access|api]. Kept in its OWN out-of-line
    // frame ([[gnu::noinline]]) with the Statement assembled by a builder in a
    // separate TU (statement_builders.cpp). Both are deliberate toolchain
    // workarounds: under the pinned Clang + -fexperimental-library, string-local
    // cleanup for this branch — when it inlines into run()'s frame — and
    // constructing the CreateTokenStmt variant alternative in this TU each
    // miscompile the parser's exception unwinding, so a thrown ParseError escapes
    // every catch and aborts. Keeping the cleanup out-of-line and the variant
    // construction out-of-TU sidesteps both. Remove when the compiler is bumped.
    [[gnu::noinline]] Statement parse_create_token() {
        expect_keyword("for");
        std::string user = parse_ident();
        std::string type = parse_token_type();
        return make_create_token_statement(std::move(user), std::move(type));
    }

    // The optional "TYPE <access|api>" tail of CREATE TOKEN; defaults to "api".
    std::string parse_token_type() {
        if (accept_keyword("type")) return parse_ident();
        return "api";
    }

    // Skip an optional "(255)" / "(10,2)" type modifier after a column type.
    void skip_type_modifier() {
        if (!accept_punct('(')) return;
        while (!at_end() && !accept_punct(')')) advance();
    }

    Statement parse_drop() {
        if (accept_keyword("database")) {
            return DropDatabaseStmt{parse_ident()};
        }
        if (accept_keyword("table")) {
            return DropTableStmt{parse_table_ref()};
        }
        if (accept_keyword("role")) {
            return parse_drop_role();
        }
        if (accept_keyword("policy")) {
            return parse_drop_policy();
        }
        throw ParseError{"expected DATABASE, TABLE, ROLE, or POLICY after DROP"};
    }

    // DROP ROLE <name>. Out-of-line + variant built in a separate TU (toolchain
    // workaround, see parse_create_token / ast.hpp).
    [[gnu::noinline]] Statement parse_drop_role() {
        return make_drop_role_statement(parse_ident());
    }

    // A permission atom from the RBAC vocabulary: `<domain>:<action>` (e.g.
    // sql:read). The ':' is a punct token, so this is ident ':' ident rejoined —
    // the executor validates the reassembled name against permission_vocabulary().
    std::string parse_permission() {
        std::string permission = parse_ident();
        expect_punct(':');
        permission += ':';
        permission += parse_ident();
        return permission;
    }

    // A comma-separated permission list (at least one).
    std::vector<std::string> parse_permission_list() {
        std::vector<std::string> permissions;
        do {
            permissions.push_back(parse_permission());
        } while (accept_punct(','));
        return permissions;
    }

    // A tool name / blob prefix: an identifier or a single-quoted string literal
    // (a blob prefix may hold characters an identifier can't).
    std::string parse_object_name() {
        if (peek().type == Tok::Str) return advance().text;
        return parse_ident();
    }

    // The `ON <object>` scope of a scoped GRANT/REVOKE (#747). Grammar:
    //   ON DATABASE <db>                       — whole database
    //   ON <db>.*                              — whole database (shorthand)
    //   ON [TABLE] <db>.<table>                — whole table
    //   ON [TABLE] <db>.<table>(col[, col]*)   — a column set
    //   ON TOOL <name>                         — an MCP tool
    //   ON BLOB <prefix>                       — a blob id / id prefix
    // Out-of-line to keep the string-heavy cleanup out of the caller's frame
    // (the toolchain EH workaround; see parse_create_token / ast.hpp).
    [[gnu::noinline]] GrantObject parse_grant_object() {
        GrantObject object;
        if (accept_keyword("tool")) {
            object.kind = GrantObjectKind::Tool;
            object.name = parse_object_name();
            return object;
        }
        if (accept_keyword("blob")) {
            object.kind = GrantObjectKind::Blob;
            object.name = parse_object_name();
            return object;
        }
        if (accept_keyword("database")) {
            object.kind = GrantObjectKind::Database;
            object.database = parse_ident();
            return object;
        }
        accept_keyword("table");  // optional keyword before db.table
        object.database = parse_ident();
        expect_punct('.');
        if (accept_punct('*')) {  // db.* — the whole database
            object.kind = GrantObjectKind::Database;
            return object;
        }
        object.table = parse_ident();
        if (accept_punct('(')) {  // a column set
            object.kind = GrantObjectKind::Columns;
            do {
                object.columns.push_back(parse_ident());
            } while (accept_punct(','));
            expect_punct(')');
        } else {
            object.kind = GrantObjectKind::Table;
        }
        return object;
    }

    // GRANT ROLE <role> TO <user>                       — assign a role to a user
    // GRANT <perm>[, <perm>]* [ON <object>] TO ROLE ...  — permissions to a role
    // Out-of-line + variants built in a separate TU (toolchain workaround, see
    // parse_create_token / ast.hpp).
    [[gnu::noinline]] Statement parse_grant() {
        if (accept_keyword("role")) {
            std::string role = parse_ident();
            expect_keyword("to");
            return make_grant_role_statement(std::move(role), parse_ident());
        }
        std::vector<std::string> permissions = parse_permission_list();
        GrantObject object;
        if (accept_keyword("on")) object = parse_grant_object();
        expect_keyword("to");
        expect_keyword("role");
        return make_grant_permission_statement(parse_ident(),
                                               std::move(permissions),
                                               std::move(object));
    }

    // REVOKE ROLE <role> FROM <user>                         — unassign a role
    // REVOKE <perm>[, <perm>]* [ON <object>] FROM ROLE ...    — permissions
    [[gnu::noinline]] Statement parse_revoke() {
        if (accept_keyword("role")) {
            std::string role = parse_ident();
            expect_keyword("from");
            return make_revoke_role_statement(std::move(role), parse_ident());
        }
        std::vector<std::string> permissions = parse_permission_list();
        GrantObject object;
        if (accept_keyword("on")) object = parse_grant_object();
        expect_keyword("from");
        expect_keyword("role");
        return make_revoke_permission_statement(parse_ident(),
                                                std::move(permissions),
                                                std::move(object));
    }

    // The command class of a CREATE POLICY (the FOR clause).
    PolicyCommand parse_policy_command() {
        if (accept_keyword("select")) return PolicyCommand::Select;
        if (accept_keyword("insert")) return PolicyCommand::Insert;
        if (accept_keyword("update")) return PolicyCommand::Update;
        if (accept_keyword("all")) return PolicyCommand::All;
        throw ParseError{"expected SELECT, INSERT, UPDATE, or ALL after FOR, got '" +
                         peek().text + "'"};
    }

    // One term of a row-security predicate: a context function (current_user() /
    // current_roles()), a boolean/null keyword, a column reference (a bare
    // identifier), or a string/number literal. Reused for both sides of the
    // comparison and for the ANY(...) argument (#748). A literal is captured as raw
    // text + a type tag (the executor decodes it) so the predicate AST stays
    // json-free and lightweight — see PolicyTerm in ast.hpp.
    PolicyTerm parse_policy_term() {
        PolicyTerm term;
        if (peek().type == Tok::Ident) {
            const std::string name = peek().text;
            if (iequals(name, "current_user")) {
                advance();
                expect_punct('(');
                expect_punct(')');
                term.kind = PolicyTerm::Kind::CurrentUser;
                return term;
            }
            if (iequals(name, "current_roles")) {
                advance();
                expect_punct('(');
                expect_punct(')');
                term.kind = PolicyTerm::Kind::CurrentRoles;
                return term;
            }
            if (iequals(name, "true") || iequals(name, "false")) {
                advance();
                term.kind = PolicyTerm::Kind::Literal;
                term.literal_type = PolicyTerm::LiteralType::Boolean;
                term.literal = iequals(name, "true") ? "true" : "false";
                return term;
            }
            if (iequals(name, "null")) {
                advance();
                term.kind = PolicyTerm::Kind::Literal;
                term.literal_type = PolicyTerm::LiteralType::Null;
                return term;
            }
            advance();  // a bare identifier is a row column reference
            term.kind = PolicyTerm::Kind::Column;
            term.column = name;
            return term;
        }
        // A numeric literal (with an optional leading '-') or a string literal.
        const bool negative = accept_punct('-');
        if (peek().type == Tok::Num) {
            term.kind = PolicyTerm::Kind::Literal;
            term.literal_type = PolicyTerm::LiteralType::Number;
            term.literal = (negative ? "-" : "") + advance().text;
            return term;
        }
        if (negative) throw ParseError{"expected a number after '-'"};
        if (peek().type == Tok::Str) {
            term.kind = PolicyTerm::Kind::Literal;
            term.literal_type = PolicyTerm::LiteralType::String;
            term.literal = advance().text;
            return term;
        }
        throw ParseError{"expected a policy predicate term (a column, a literal, "
                         "current_user(), or current_roles()), got '" +
                         peek().text + "'"};
    }

    // The parenthesized predicate of a USING / WITH CHECK clause: `(true)` (a
    // permissive filter), or a single comparison `(<term> <op> <term>)` where the
    // RHS may be `ANY(<term>)` for a membership test — e.g.
    //   (owner = current_user())
    //   ('admin' = ANY(current_roles()))
    // Out-of-line to keep the json/string-heavy cleanup out of the caller's frame
    // (the toolchain EH workaround; see parse_create_token / ast.hpp).
    [[gnu::noinline]] PolicyPredicate parse_policy_predicate() {
        expect_punct('(');
        PolicyPredicate predicate;
        predicate.lhs = parse_policy_term();
        // `(true)` — a bare boolean literal admits every row.
        if (predicate.lhs.kind == PolicyTerm::Kind::Literal &&
            predicate.lhs.literal_type == PolicyTerm::LiteralType::Boolean &&
            predicate.lhs.literal == "true" && peek().type == Tok::Punct &&
            peek().text == ")") {
            predicate.always_true = true;
            expect_punct(')');
            return predicate;
        }
        predicate.op = parse_comparison();
        if (accept_keyword("any")) {
            expect_punct('(');
            predicate.rhs = parse_policy_term();
            expect_punct(')');
            predicate.rhs_any = true;
        } else {
            predicate.rhs = parse_policy_term();
        }
        expect_punct(')');
        return predicate;
    }

    // CREATE POLICY <name> ON <db.table> FOR SELECT|INSERT|UPDATE|ALL
    //   [TO ROLE <role>[, <role>]*] USING (<predicate>) [WITH CHECK (<predicate>)]
    // Out-of-line + variant built in a separate TU (toolchain workaround, see
    // parse_create_token / ast.hpp).
    [[gnu::noinline]] Statement parse_create_policy() {
        std::string name = parse_ident();
        expect_keyword("on");
        TableRef table = parse_table_ref();
        expect_keyword("for");
        PolicyCommand command = parse_policy_command();
        std::vector<std::string> roles;
        if (accept_keyword("to")) {
            expect_keyword("role");
            do {
                roles.push_back(parse_ident());
            } while (accept_punct(','));
        }
        std::optional<PolicyPredicate> using_predicate;
        std::optional<PolicyPredicate> check_predicate;
        if (accept_keyword("using")) using_predicate = parse_policy_predicate();
        if (accept_keyword("with")) {
            expect_keyword("check");
            check_predicate = parse_policy_predicate();
        }
        return make_create_policy_statement(
            std::move(name), std::move(table), command, std::move(roles),
            std::move(using_predicate), std::move(check_predicate));
    }

    // DROP POLICY <name> [ON <db.table>]. The optional ON clause is parsed for
    // symmetry with CREATE and ignored (policy names are unique). Out-of-line +
    // variant built in a separate TU (toolchain workaround).
    [[gnu::noinline]] Statement parse_drop_policy() {
        std::string name = parse_ident();
        if (accept_keyword("on")) parse_table_ref();  // accepted, ignored
        return make_drop_policy_statement(std::move(name));
    }

    Statement parse_insert() {
        expect_keyword("into");
        InsertStmt stmt;
        stmt.table = parse_table_ref();
        if (accept_punct('(')) {
            do {
                stmt.columns.push_back(parse_ident());
            } while (accept_punct(','));
            expect_punct(')');
        }
        expect_keyword("values");
        do {
            expect_punct('(');
            std::vector<nlohmann::json> tuple;
            if (!accept_punct(')')) {
                do {
                    tuple.push_back(parse_value());
                } while (accept_punct(','));
                expect_punct(')');
            }
            stmt.rows.push_back(std::move(tuple));
        } while (accept_punct(','));
        return stmt;
    }

    Statement parse_select() {
        SelectStmt stmt;

        // The select list. Each item is `*`, a bare column, or an aggregate call
        // FUNC(arg), each with an optional `AS alias`. Aggregates and key columns
        // are collected into `items` (in select order); their presence + a GROUP BY
        // clause decide whether this is an aggregating query.
        std::vector<GroupedItem> items;
        bool any_aggregate = false;
        if (accept_punct('*')) {
            stmt.all = true;
        } else {
            do {
                GroupedItem item;
                if (at_aggregate_call()) {
                    AggregateExpr aggregate;
                    aggregate.func = *aggregate_func_of(advance().text);
                    expect_punct('(');
                    if (accept_punct('*')) {
                        // COUNT(*) — no column argument.
                        if (aggregate.func != AggregateFunc::Count) {
                            throw ParseError{"only COUNT(*) takes '*'; other "
                                             "aggregates need a column"};
                        }
                    } else {
                        aggregate.column = parse_ident();
                    }
                    expect_punct(')');
                    item.aggregate = std::move(aggregate);
                    any_aggregate = true;
                } else {
                    item.column = parse_ident();
                }
                if (accept_keyword("as")) item.alias = parse_ident();
                items.push_back(std::move(item));
            } while (accept_punct(','));
        }
        expect_keyword("from");
        stmt.table = parse_table_ref();
        if (accept_keyword("where")) {
            WhereClause where;
            where.column = parse_ident();
            where.op = parse_comparison();
            where.value = parse_value();
            stmt.where = std::move(where);
        }
        if (accept_keyword("group")) {
            expect_keyword("by");
            do {
                stmt.group_by.push_back(parse_ident());
            } while (accept_punct(','));
        }

        // Reconcile the select list with the GROUP BY into either a plain or an
        // aggregating SELECT. An aggregate function requires GROUP BY; `SELECT *`
        // cannot be grouped.
        if (any_aggregate && stmt.group_by.empty()) {
            throw ParseError{"aggregate functions require a GROUP BY clause"};
        }
        if (stmt.all && !stmt.group_by.empty()) {
            throw ParseError{"SELECT * cannot be combined with GROUP BY"};
        }
        if (!stmt.group_by.empty()) {
            stmt.items = std::move(items);  // grouped: the ordered key/aggregate list
        } else if (!stmt.all) {
            // Plain projection: every item is a bare column (no aggregates — the
            // GROUP BY check above already guarantees that). AS rename is only
            // supported on the grouped path, so reject it here rather than drop it.
            for (const GroupedItem& item : items) {
                if (!item.alias.empty()) {
                    throw ParseError{"column aliases (AS) are only supported in "
                                     "GROUP BY queries"};
                }
                stmt.columns.push_back(item.column);
            }
        }
        return stmt;
    }

    std::string_view src_;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

}  // namespace

ParseResult PostgresDialect::parse(std::string_view sql) const {
    return Parser(sql).run();
}

}  // namespace zetta::query
