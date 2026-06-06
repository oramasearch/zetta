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
            static constexpr std::string_view kPunct = "(),;*.=-";
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
        throw ParseError{"unsupported statement, expected CREATE/DROP/INSERT/"
                         "SELECT, got '" + peek().text + "'"};
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
        throw ParseError{"expected DATABASE or TABLE after CREATE"};
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
        throw ParseError{"expected DATABASE or TABLE after DROP"};
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
        if (accept_punct('*')) {
            stmt.all = true;
        } else {
            do {
                stmt.columns.push_back(parse_ident());
            } while (accept_punct(','));
        }
        expect_keyword("from");
        stmt.table = parse_table_ref();
        if (accept_keyword("where")) {
            WhereEq where;
            where.column = parse_ident();
            expect_punct('=');
            where.value = parse_value();
            stmt.where = std::move(where);
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
