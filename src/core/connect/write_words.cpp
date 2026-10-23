#include "write_words.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace zetta::connect {
namespace {

bool is_word_start(char character) {
    return std::isalpha(static_cast<unsigned char>(character)) || character == '_';
}

bool is_word_char(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_' ||
           character == '$';
}

bool is_space(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return out;
}

// The bare words of `sql`, lower-cased, in order: everything outside a '…'
// string, a "…" identifier or string, a `-- ` comment and a `/* … */` comment.
// With `backslash_escapes`, a backslash inside a '…' or "…" run escapes the next
// character. With `identifier_quotes`, `…` (a doubled backtick escapes one) and
// […] are identifiers too, as SQLite and MySQL read them: a quote inside one does
// not open a string, so a write after it is never hidden (`WITH [a'b] AS (SELECT 1)
// DELETE FROM t --'`).
std::vector<std::string> bare_words(std::string_view sql, bool backslash_escapes,
                                    bool identifier_quotes) {
    std::vector<std::string> words;
    std::size_t index = 0;
    while (index < sql.size()) {
        const char character = sql[index];
        if (identifier_quotes && (character == '`' || character == '[')) {
            const char close = character == '[' ? ']' : '`';
            ++index;
            while (index < sql.size()) {
                if (sql[index] == close) {
                    if (close == '`' && index + 1 < sql.size() && sql[index + 1] == '`') {
                        index += 2;
                        continue;
                    }
                    break;
                }
                ++index;
            }
            ++index;
            continue;
        }
        if (character == '\'' || character == '"') {
            ++index;
            while (index < sql.size()) {
                if (backslash_escapes && sql[index] == '\\') {
                    index += 2;
                    continue;
                }
                if (sql[index] == character) {
                    // A doubled quote is an escaped quote, not the end.
                    if (index + 1 < sql.size() && sql[index + 1] == character) {
                        index += 2;
                        continue;
                    }
                    break;
                }
                ++index;
            }
            ++index;
            continue;
        }
        if (sql.substr(index).starts_with("--") &&
            (index + 2 == sql.size() || is_space(sql[index + 2]))) {
            const std::size_t line_end = sql.find('\n', index);
            index = line_end == std::string_view::npos ? sql.size() : line_end + 1;
            continue;
        }
        if (sql.substr(index).starts_with("/*")) {
            const std::size_t close = sql.find("*/", index + 2);
            index = close == std::string_view::npos ? sql.size() : close + 2;
            continue;
        }
        if (is_word_start(character) && (index == 0 || !is_word_char(sql[index - 1]))) {
            std::size_t end = index;
            while (end < sql.size() && is_word_char(sql[end])) ++end;
            words.push_back(lowered(sql.substr(index, end - index)));
            index = end;
            continue;
        }
        ++index;
    }
    return words;
}

// Words that start a write, change the schema, touch files or extensions, or
// change the session or transaction, in SQLite, DuckDB, Polars SQL, MySQL and
// TiDB.
constexpr std::array<std::string_view, 30> kCommonWords = {
    "insert",  "update",   "delete",     "merge",      "upsert",  "create",
    "drop",    "alter",    "truncate",   "attach",     "detach",  "pragma",
    "vacuum",  "reindex",  "analyze",    "copy",       "install", "load",
    "export",  "import",   "call",       "checkpoint", "set",     "reset",
    "begin",   "commit",   "rollback",   "savepoint",  "grant",   "revoke"};

// MySQL and TiDB statements beyond those.
constexpr std::array<std::string_view, 18> kMysqlWords = {
    "handler", "lock",      "unlock",     "rename",  "flush",   "kill",
    "shutdown", "optimize", "repair",     "purge",   "prepare", "execute",
    "deallocate", "uninstall", "flashback", "recover", "split", "admin"};

std::string first_refused(const std::vector<std::string>& words,
                          WriteWordDialect dialect) {
    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::string& word = words[index];
        if (std::ranges::find(kCommonWords, word) != kCommonWords.end()) return word;
        if (dialect == WriteWordDialect::Mysql &&
            std::ranges::find(kMysqlWords, word) != kMysqlWords.end()) {
            return word;
        }
        // `replace(…)` is a string function; `REPLACE INTO` is a write.
        if (word == "replace" && index + 1 < words.size() && words[index + 1] == "into") {
            return word;
        }
    }
    return {};
}

}  // namespace

std::string write_word_rejection(std::string_view sql, WriteWordDialect dialect,
                                 std::string_view through) {
    // Every reading an engine could take of the quoting: a word any of them sees
    // bare is refused.
    std::string word;
    for (const bool identifier_quotes : {false, true}) {
        for (const bool backslash_escapes : {false, true}) {
            if (word.empty()) {
                word = first_refused(bare_words(sql, backslash_escapes, identifier_quotes),
                                     dialect);
            }
        }
    }
    if (word.empty()) return {};
    return "'" + word + "' may not appear in a query through " + std::string(through) +
           " (it writes or changes the session); only a read-only SELECT may run — "
           "quote an identifier that has this name";
}

}  // namespace zetta::connect
