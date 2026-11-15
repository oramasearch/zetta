#include "zetta/query/postgres.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::query {
namespace {

// The parser never throws (#758, #793, #819). A statement from any client
// reaches it, and a throw here once escaped the parser's own catch (libc++abi
// paired with the wrong unwinder; see the CMakeLists note) and ended the node
// with std::terminate. Whether a catch works must not depend on how the binary
// was linked, so a parse failure is a value, not an exception: fail() records
// the first error, with where it happened, and parks the cursor on the End token. Every
// caller up the recursive descent then falls through on its own: accept_* never
// matches at End, every list loop stops, expect_* and parse_ident add nothing
// (the first error stands), and run() discards the half-built statements. A
// function that fails returns an empty value, which nothing uses.

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
    std::size_t offset = 0;  // byte offset of the token's first character
};

class Parser {
public:
    explicit Parser(std::string_view sql) : src_(sql) {}

    ParseResult run() {
        ParseResult result;
        tokenize();
        while (!failed_ && !at_end()) {
            if (accept_punct(';')) continue;  // stray/empty statement
            Statement statement = parse_statement();
            if (failed_) break;
            result.statements.push_back(std::move(statement));
            if (!at_end() && !accept_punct(';')) {
                fail("expected ';' after statement, got '" + peek().text + "'");
            }
        }
        if (failed_) {
            result.statements.clear();
            result.error = error_ + where_suffix();
        }
        return result;
    }

private:
    // Record the first parse error and stop the parse (see the note at the top).
    // A tokenizer error has already set error_offset_; any other error is at the
    // token the cursor is on now.
    void fail(std::string message) {
        if (failed_) return;
        failed_ = true;
        error_ = std::move(message);
        if (!error_offset_ && pos_ < tokens_.size()) {
            error_offset_ = tokens_[pos_].offset;
            error_near_ = tokens_[pos_].text;
        }
        if (!tokens_.empty()) pos_ = tokens_.size() - 1;  // the End token
    }

    // Where a parse error happened, as Postgres reports it (#1218): the 1-based
    // character position of the offending token and the token itself, so a
    // caller can point at it.
    std::string where_suffix() const {
        const std::size_t offset = error_offset_.value_or(src_.size());
        if (offset >= src_.size()) return " (at end of input)";
        std::string suffix = " (at position " + std::to_string(offset + 1);
        if (!error_near_.empty()) suffix += ", near '" + error_near_ + "'";
        return suffix + ")";
    }

    // --- tokenizer ----------------------------------------------------------
    // On a bad character or an unterminated string it records the error at that
    // offset and leaves tokens_ as the End token alone.
    void tokenize() {
        const std::string_view s = src_;
        const std::size_t n = s.size();
        const auto ident_start = [](char c) {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
        };
        const auto ident_char = [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        };
        const auto tokenizer_error = [&](std::size_t at, std::string message) {
            tokens_.clear();
            tokens_.push_back({Tok::End, "", n});
            error_offset_ = at;
            fail(std::move(message));
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
                tokens_.push_back({Tok::Ident, std::string(s.substr(i, j - i)), i});
                i = j;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                std::size_t j = i + 1;
                while (j < n && (std::isdigit(static_cast<unsigned char>(s[j])) ||
                                 s[j] == '.')) {
                    ++j;
                }
                tokens_.push_back({Tok::Num, std::string(s.substr(i, j - i)), i});
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
                if (!closed) {
                    tokenizer_error(i, "unterminated string literal");
                    return;
                }
                tokens_.push_back({Tok::Str, std::move(lit), i});
                i = j;
                continue;
            }
            static constexpr std::string_view kPunct = "(),;*.=-<>!:";
            if (kPunct.find(c) != std::string_view::npos) {
                tokens_.push_back({Tok::Punct, std::string(1, c), i});
                ++i;
                continue;
            }
            tokenizer_error(i, std::string("unexpected character '") + c + "'");
            return;
        }
        tokens_.push_back({Tok::End, "", n});
    }

    // --- cursor -------------------------------------------------------------
    // The cursor never moves past the End token, so peek() is always valid.
    const Token& peek() const { return tokens_[pos_]; }
    const Token& advance() {
        const Token& token = tokens_[pos_];
        if (pos_ + 1 < tokens_.size()) ++pos_;
        return token;
    }
    bool at_end() const { return peek().type == Tok::End; }

    bool at_keyword(std::string_view kw) const {
        return peek().type == Tok::Ident && iequals(peek().text, kw);
    }
    bool accept_keyword(std::string_view kw) {
        if (peek().type == Tok::Ident && iequals(peek().text, kw)) {
            advance();
            return true;
        }
        return false;
    }
    void expect_keyword(std::string_view kw) {
        if (!accept_keyword(kw)) {
            fail("expected '" + std::string(kw) + "', got '" + peek().text + "'");
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
            fail(std::string("expected '") + c + "', got '" + peek().text + "'");
        }
    }
    std::string parse_ident() {
        if (peek().type != Tok::Ident) {
            fail("expected an identifier, got '" + peek().text + "'");
            return {};
        }
        return advance().text;
    }

    // A non-negative whole number, for LIMIT and OFFSET.
    std::size_t parse_count(std::string_view clause) {
        if (peek().type != Tok::Num ||
            peek().text.find('.') != std::string::npos) {
            fail(std::string(clause) + " takes a non-negative whole number, got '" +
                 peek().text + "'");
            return 0;
        }
        const std::string text = advance().text;
        std::size_t count = 0;
        const char* const last = text.data() + text.size();
        const auto [end, error] = std::from_chars(text.data(), last, count);
        if (error != std::errc{} || end != last) {
            fail(std::string(clause) + " is out of range: " + text);
            return 0;
        }
        return count;
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
        fail("expected a comparison operator (=, !=, <>, <, <=, >, >=), got '" +
             peek().text + "'");
        return CompareOp::Equal;
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
                fail("invalid number: " + num);
                return nullptr;
            }
            advance();
            return v;
        }
        if (negative) {
            fail("expected a number after '-'");
            return nullptr;
        }
        if (t.type == Tok::Str) {
            nlohmann::json text = t.text;
            advance();
            return text;
        }
        if (t.type == Tok::Ident) {
            if (iequals(t.text, "true")) { advance(); return true; }
            if (iequals(t.text, "false")) { advance(); return false; }
            if (iequals(t.text, "null")) { advance(); return nullptr; }
        }
        fail("expected a value, got '" + t.text + "'");
        return nullptr;
    }

    // --- statements ---------------------------------------------------------
    Statement parse_statement() {
        if (accept_keyword("create")) return parse_create();
        if (accept_keyword("drop")) return parse_drop();
        if (accept_keyword("insert")) return parse_insert();
        if (accept_keyword("update")) return parse_update();
        if (accept_keyword("delete")) return parse_delete();
        if (accept_keyword("truncate")) return parse_truncate();
        if (accept_keyword("select")) return parse_select();
        if (accept_keyword("grant")) return parse_grant();
        if (accept_keyword("revoke")) return parse_revoke();
        fail("unsupported statement, expected CREATE/DROP/INSERT/UPDATE/DELETE/"
             "TRUNCATE/SELECT/GRANT/REVOKE, got '" + peek().text + "'");
        return {};
    }

    // The WHERE predicate shared by SELECT, UPDATE and DELETE (#912): comparisons
    // `<column> <op> <value>` combined with AND, OR, NOT and parentheses, with
    // SQL's precedence — NOT binds tightest, then AND, then OR — so
    // `a = 1 OR b = 2 AND c = 3` is `a = 1 OR (b = 2 AND c = 3)`. A run of the
    // same operator is one node with every operand (`a AND b AND c`).
    WhereClause parse_where_clause() { return parse_where_or(); }

    WhereClause parse_where_or() {
        WhereClause first = parse_where_and();
        if (!at_keyword("or")) return first;
        WhereClause any;
        any.kind = WhereClause::Kind::Or;
        any.operands.push_back(std::move(first));
        while (accept_keyword("or")) any.operands.push_back(parse_where_and());
        return any;
    }

    WhereClause parse_where_and() {
        WhereClause first = parse_where_not();
        if (!at_keyword("and")) return first;
        WhereClause all;
        all.kind = WhereClause::Kind::And;
        all.operands.push_back(std::move(first));
        while (accept_keyword("and")) all.operands.push_back(parse_where_not());
        return all;
    }

    WhereClause parse_where_not() {
        if (accept_keyword("not")) {
            WhereClause negation;
            negation.kind = WhereClause::Kind::Not;
            negation.operands.push_back(parse_where_not());
            return negation;
        }
        if (accept_punct('(')) {
            WhereClause inner = parse_where_or();
            expect_punct(')');
            return inner;
        }
        WhereClause comparison;
        comparison.column = parse_ident();
        comparison.op = parse_comparison();
        comparison.value = parse_value();
        return comparison;
    }

    Statement parse_update() {
        UpdateStmt stmt;
        stmt.table = parse_table_ref();
        expect_keyword("set");
        do {
            Assignment assignment;
            assignment.column = parse_ident();
            expect_punct('=');
            assignment.value = parse_value();
            stmt.assignments.push_back(std::move(assignment));
        } while (accept_punct(','));
        if (accept_keyword("where")) stmt.where = parse_where_clause();
        return stmt;
    }

    Statement parse_delete() {
        expect_keyword("from");
        DeleteStmt stmt;
        stmt.table = parse_table_ref();
        if (accept_keyword("where")) stmt.where = parse_where_clause();
        return stmt;
    }

    Statement parse_truncate() {
        accept_keyword("table");  // optional TABLE keyword (TRUNCATE t == TRUNCATE TABLE t)
        TruncateStmt stmt;
        stmt.table = parse_table_ref();
        return stmt;
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
        if (accept_keyword("virtual")) {
            expect_keyword("table");
            return parse_create_virtual_table();
        }
        if (accept_keyword("user")) {
            return CreateUserStmt{parse_ident()};
        }
        if (accept_keyword("token")) {
            return parse_create_token();
        }
        if (accept_keyword("role")) {
            return CreateRoleStmt{parse_ident()};
        }
        if (accept_keyword("policy")) {
            return parse_create_policy();
        }
        fail("expected DATABASE, TABLE, VIRTUAL TABLE, USER, TOKEN, ROLE, or "
             "POLICY after CREATE");
        return {};
    }

    // CREATE VIRTUAL TABLE (#809 slice 3):
    //   <name> OVER CRDT '<prefix>' ( <col> <type> <tier…>, … )
    // OVER CRDT is REQUIRED (the must-extend-a-CRDT invariant).
    Statement parse_create_virtual_table() {
        CreateVirtualTableStmt stmt;
        if (peek().type != Tok::Ident) {
            fail("expected a virtual-table name, got '" + peek().text + "'");
            return {};
        }
        stmt.name = advance().text;
        if (!accept_keyword("over") || !accept_keyword("crdt")) {
            fail("CREATE VIRTUAL TABLE requires OVER CRDT '<prefix>' (a virtual "
                 "table must extend a CRDT key-family), got '" + peek().text + "'");
            return {};
        }
        if (peek().type != Tok::Str) {
            fail("expected a quoted CRDT key prefix after OVER CRDT, got '" +
                 peek().text + "'");
            return {};
        }
        stmt.crdt_prefix = advance().text;
        if (!accept_punct('(')) {
            fail("expected '(' before the virtual-table columns, got '" +
                 peek().text + "'");
            return {};
        }
        if (accept_punct(')')) return stmt;
        do {
            VirtualColumnDef column;
            if (peek().type != Tok::Ident) {
                fail("expected a column name, got '" + peek().text + "'");
                return {};
            }
            column.name = advance().text;
            if (peek().type != Tok::Ident) {
                fail("expected a type for column '" + column.name + "', got '" +
                     peek().text + "'");
                return {};
            }
            column.type = advance().text;  // a type is required (e.g. TEXT)
            if (accept_keyword("durable")) {
                column.tier = VirtualTableTier::Durable;
                if (accept_keyword("key")) column.is_key = true;
            } else if (accept_keyword("ephemeral")) {
                if (accept_keyword("local")) {
                    column.tier = VirtualTableTier::EphemeralLocal;
                } else if (accept_keyword("synced")) {
                    column.tier = VirtualTableTier::EphemeralSynced;
                } else {
                    fail("expected LOCAL or SYNCED after EPHEMERAL for column '" +
                         column.name + "'");
                    return {};
                }
            } else {
                fail("expected a tier (DURABLE [KEY], EPHEMERAL LOCAL, EPHEMERAL "
                     "SYNCED) for column '" + column.name + "'");
                return {};
            }
            stmt.columns.push_back(std::move(column));
        } while (accept_punct(','));
        if (!accept_punct(')')) {
            fail("expected ',' or ')' in the virtual-table columns, got '" +
                 peek().text + "'");
            return {};
        }
        return stmt;
    }

    // CREATE TOKEN FOR <user> [TYPE access|api]; the type defaults to "api".
    Statement parse_create_token() {
        expect_keyword("for");
        CreateTokenStmt stmt;
        stmt.user = parse_ident();
        stmt.type = accept_keyword("type") ? parse_ident() : std::string("api");
        return stmt;
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
            return DropRoleStmt{parse_ident()};
        }
        if (accept_keyword("policy")) {
            return parse_drop_policy();
        }
        fail("expected DATABASE, TABLE, ROLE, or POLICY after DROP");
        return {};
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
    //   ON COLLECTION <prefix>                 — a record collection name prefix
    //   ON BUCKET <prefix>                     — an object bucket name prefix
    //   ON CONNECTION <name>                   — one saved source connection
    GrantObject parse_grant_object() {
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
        if (accept_keyword("collection")) {  // a record collection name prefix (#1231)
            object.kind = GrantObjectKind::Collection;
            object.name = parse_object_name();
            return object;
        }
        if (accept_keyword("bucket")) {  // an object bucket name prefix (#1005)
            object.kind = GrantObjectKind::Bucket;
            object.name = parse_object_name();
            return object;
        }
        if (accept_keyword("connection")) {  // one saved source connection (#1339)
            object.kind = GrantObjectKind::Connection;
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
    Statement parse_grant() {
        if (accept_keyword("role")) {
            GrantRoleStmt stmt;
            stmt.role = parse_ident();
            expect_keyword("to");
            stmt.user = parse_ident();
            return stmt;
        }
        GrantPermissionStmt stmt;
        stmt.permissions = parse_permission_list();
        if (accept_keyword("on")) stmt.object = parse_grant_object();
        expect_keyword("to");
        expect_keyword("role");
        stmt.role = parse_ident();
        return stmt;
    }

    // REVOKE ROLE <role> FROM <user>                         — unassign a role
    // REVOKE <perm>[, <perm>]* [ON <object>] FROM ROLE ...    — permissions
    Statement parse_revoke() {
        if (accept_keyword("role")) {
            RevokeRoleStmt stmt;
            stmt.role = parse_ident();
            expect_keyword("from");
            stmt.user = parse_ident();
            return stmt;
        }
        RevokePermissionStmt stmt;
        stmt.permissions = parse_permission_list();
        if (accept_keyword("on")) stmt.object = parse_grant_object();
        expect_keyword("from");
        expect_keyword("role");
        stmt.role = parse_ident();
        return stmt;
    }

    // The command class of a CREATE POLICY (the FOR clause).
    PolicyCommand parse_policy_command() {
        if (accept_keyword("select")) return PolicyCommand::Select;
        if (accept_keyword("insert")) return PolicyCommand::Insert;
        if (accept_keyword("update")) return PolicyCommand::Update;
        if (accept_keyword("all")) return PolicyCommand::All;
        fail("expected SELECT, INSERT, UPDATE, or ALL after FOR, got '" +
             peek().text + "'");
        return PolicyCommand::Select;
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
        if (negative) {
            fail("expected a number after '-'");
            return term;
        }
        if (peek().type == Tok::Str) {
            term.kind = PolicyTerm::Kind::Literal;
            term.literal_type = PolicyTerm::LiteralType::String;
            term.literal = advance().text;
            return term;
        }
        fail("expected a policy predicate term (a column, a literal, "
             "current_user(), or current_roles()), got '" + peek().text + "'");
        return term;
    }

    // The parenthesized predicate of a USING / WITH CHECK clause: `(true)` (a
    // permissive filter), or a single comparison `(<term> <op> <term>)` where the
    // RHS may be `ANY(<term>)` for a membership test — e.g.
    //   (owner = current_user())
    //   ('admin' = ANY(current_roles()))
    PolicyPredicate parse_policy_predicate() {
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
    Statement parse_create_policy() {
        CreatePolicyStmt stmt;
        stmt.name = parse_ident();
        expect_keyword("on");
        stmt.table = parse_table_ref();
        expect_keyword("for");
        stmt.command = parse_policy_command();
        if (accept_keyword("to")) {
            expect_keyword("role");
            do {
                stmt.roles.push_back(parse_ident());
            } while (accept_punct(','));
        }
        if (accept_keyword("using")) stmt.using_predicate = parse_policy_predicate();
        if (accept_keyword("with")) {
            expect_keyword("check");
            stmt.check_predicate = parse_policy_predicate();
        }
        return stmt;
    }

    // DROP POLICY <name> [ON <db.table>]. The optional ON clause is parsed for
    // symmetry with CREATE and ignored (policy names are unique).
    Statement parse_drop_policy() {
        DropPolicyStmt stmt;
        stmt.name = parse_ident();
        if (accept_keyword("on")) parse_table_ref();  // accepted, ignored
        return stmt;
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
                            fail("only COUNT(*) takes '*'; other aggregates need "
                                 "a column");
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
        if (accept_keyword("where")) stmt.where = parse_where_clause();
        if (accept_keyword("group")) {
            expect_keyword("by");
            do {
                stmt.group_by.push_back(parse_ident());
            } while (accept_punct(','));
        }
        if (accept_keyword("order")) {
            expect_keyword("by");
            do {
                OrderKey key;
                key.column = parse_ident();
                if (accept_keyword("desc")) {
                    key.descending = true;
                } else {
                    accept_keyword("asc");
                }
                stmt.order_by.push_back(std::move(key));
            } while (accept_punct(','));
        }
        // LIMIT n [OFFSET m] or OFFSET m [LIMIT n], as Postgres takes either order,
        // and the SQL-standard spelling `[OFFSET m {ROW|ROWS}] FETCH {FIRST|NEXT}
        // [n] {ROW|ROWS} ONLY` (n defaults to 1). A statement says how many rows
        // once: LIMIT and FETCH together are refused, as Postgres refuses them.
        bool limit_seen = false;
        bool offset_seen = false;
        for (int clause = 0; clause < 2; ++clause) {
            if (!limit_seen && accept_keyword("limit")) {
                limit_seen = true;
                if (accept_keyword("all")) continue;
                stmt.limit = parse_count("LIMIT");
            } else if (!offset_seen && accept_keyword("offset")) {
                offset_seen = true;
                stmt.offset = parse_count("OFFSET");
                if (!accept_keyword("rows")) accept_keyword("row");
            } else if (!limit_seen && accept_keyword("fetch")) {
                limit_seen = true;
                if (!accept_keyword("first")) expect_keyword("next");
                std::size_t count = 1;
                if (peek().type == Tok::Num) count = parse_count("FETCH FIRST");
                if (!accept_keyword("rows")) expect_keyword("row");
                expect_keyword("only");
                stmt.limit = count;
            }
        }
        if (peek().type == Tok::Ident &&
            (iequals(peek().text, "limit") || iequals(peek().text, "fetch"))) {
            fail("the row count is given twice (use LIMIT or FETCH FIRST, once)");
            return {};
        }

        // Reconcile the select list with the GROUP BY into either a plain or an
        // aggregating SELECT. An aggregate with no GROUP BY aggregates the whole
        // table as one group (`SELECT count(*) FROM t`, #1218); `SELECT *`
        // cannot be grouped. Whether a bare column may sit beside an aggregate is
        // the executor's call (it must be the GROUP BY key).
        if (stmt.all && !stmt.group_by.empty()) {
            fail("SELECT * cannot be combined with GROUP BY");
            return {};
        }
        if (any_aggregate || !stmt.group_by.empty()) {
            stmt.items = std::move(items);  // the ordered key/aggregate list
        } else if (!stmt.all) {
            // Plain projection: every item is a bare column (an aggregate would
            // have made this an aggregating query above). AS rename is only
            // supported on the grouped path, so reject it here rather than drop it.
            for (const GroupedItem& item : items) {
                if (!item.alias.empty()) {
                    fail("column aliases (AS) are only supported in GROUP BY "
                         "queries");
                    return {};
                }
                stmt.columns.push_back(item.column);
            }
        }
        return stmt;
    }

    std::string_view src_;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    // The first parse error (see fail()), and where it is: the source offset and
    // the token text there. A tokenizer error sets the offset itself.
    bool failed_ = false;
    std::string error_;
    std::optional<std::size_t> error_offset_;
    std::string error_near_;
};

}  // namespace

ParseResult PostgresDialect::parse(std::string_view sql) const {
    return Parser(sql).run();
}

}  // namespace zetta::query
