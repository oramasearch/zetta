#include "zetta/input/python_frontend.hpp"

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"



namespace zetta::input {
namespace {
struct Expr;

using ir::EdgeKind;
using ir::ExecutionGraph;
using ir::NodeId;
using ir::StepKind;

// The whole frontend is **non-throwing**: a parse failure is recorded as an error
// string and propagated by bailing out, never by an exception. This is zetta's
// convention for zetta-core (ir::from_json / compile_n8n_workflow / compile_intent
// are all non-throwing) — the library is compiled with -fexperimental-library and
// must not unwind exceptions across its boundary.

// ===========================================================================
// Tokens
// ===========================================================================

enum class Tok {
    Name,     // identifier or keyword (True/False/None/import/from/as/def/…)
    Number,   // integer or float literal text
    String,   // decoded string literal contents
    Op,       // one punctuation char: ( ) [ ] { } . , = : ; and unsupported ops
    Newline,  // a logical statement separator
    End,
};

struct Token {
    Tok type;
    std::string text;  // identifier/number text, decoded string, or the op char
};

// ===========================================================================
// AST — only what the subset needs
// ===========================================================================

enum class ExprKind {
    Name,       // a bare identifier (variable reference)
    Literal,    // a string / number / bool / None literal (value in `literal`)
    List,       // [ items… ]
    Dict,       // { key: value, … }
    Attribute,  // base.attribute   (not a call)
    Subscript,  // base[index]
    Call,       // callee(args…)    (callee == `base`)
};

using ExprPtr = std::unique_ptr<Expr>;

// One call argument: positional (keyword empty) or keyword (`name=value`).
struct Arg {
    std::string keyword;  // empty for a positional argument
    ExprPtr value;
};

struct Expr {
    ExprKind kind;
    std::string name;         // Name: the identifier
    nlohmann::json literal;   // Literal: the value
    std::string attribute;    // Attribute: the attribute / method name
    ExprPtr base;             // Attribute/Subscript: receiver; Call: callee
    std::vector<ExprPtr> items;                       // List items
    std::vector<std::pair<ExprPtr, ExprPtr>> entries; // Dict key/value
    ExprPtr index;            // Subscript: the index expression
    std::vector<Arg> args;    // Call arguments
};

ExprPtr make_expr(ExprKind kind) {
    auto expr = std::make_unique<Expr>();
    expr->kind = kind;
    return expr;
}

// A parsed statement: either an assignment (target = value) or a bare expression.
// import/from-import lines are consumed and produce nothing (no step), so they
// carry a null `value`.
struct Statement {
    std::string target;  // assignment target; empty for a bare expression
    ExprPtr value;       // the right-hand side / the bare expression (null = skip)
};

// The outcome of parsing: the statement list, or an error reason (mutually
// exclusive). Mirrors query::ParseResult.
struct ParsedProgram {
    std::vector<Statement> statements;
    std::string error;  // empty on success
};

// Python keywords this subset deliberately does NOT support — encountering one is
// a clean, explicit rejection rather than a confusing parse error.
bool is_unsupported_keyword(std::string_view word) {
    static constexpr std::string_view kUnsupported[] = {
        "def",    "class",  "if",     "elif",   "else",   "for",
        "while",  "with",   "try",    "except", "finally", "return",
        "yield",  "lambda", "async",  "await",  "global", "nonlocal",
        "raise",  "assert", "del",    "pass",   "break",  "continue"};
    for (const std::string_view candidate : kUnsupported) {
        if (word == candidate) return true;
    }
    return false;
}

// Parse a numeric literal token into a JSON number (int when it has no '.'/'e').
// On a malformed token it sets `*ok` false and returns 0. Uses the non-throwing
// strtod/strtoll (not stod/stoll) so the whole frontend stays exception-free.
nlohmann::json parse_number(const std::string& raw, bool* ok) {
    *ok = true;
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (const char character : raw) {
        if (character != '_') cleaned.push_back(character);
    }
    const bool is_float = cleaned.find('.') != std::string::npos ||
                          cleaned.find('e') != std::string::npos ||
                          cleaned.find('E') != std::string::npos;
    const char* begin = cleaned.c_str();
    char* end = nullptr;
    errno = 0;
    if (is_float) {
        const double value = std::strtod(begin, &end);
        if (end == begin || *end != '\0' || errno != 0) {
            *ok = false;
            return nlohmann::json(0);
        }
        return nlohmann::json(value);
    }
    const long long value = std::strtoll(begin, &end, 10);
    if (end == begin || *end != '\0' || errno != 0) {
        *ok = false;
        return nlohmann::json(0);
    }
    return nlohmann::json(value);
}

// ===========================================================================
// Parser — recursive descent over the source into the AST, non-throwing
// ===========================================================================

class Parser {
public:
    explicit Parser(std::string_view source) : source_(source) {}

    // Tokenize then parse; on the first error, `error_` is set and parsing bails.
    ParsedProgram run() {
        tokenize();
        std::vector<Statement> statements;
        while (!failed() && !at_end()) {
            if (accept_newline_or_semicolon()) continue;  // blank line / `;;`
            Statement statement = parse_statement();
            if (failed()) break;
            if (statement.value || !statement.target.empty()) {
                statements.push_back(std::move(statement));
            }
            // A statement ends at a newline, a `;`, or end of input.
            if (!at_end() && !accept_newline_or_semicolon()) {
                fail("unexpected '" + peek().text + "' after statement");
                break;
            }
        }
        if (failed()) return ParsedProgram{{}, error_};
        return ParsedProgram{std::move(statements), {}};
    }

private:
    // --- error state -------------------------------------------------------
    void fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
    }
    bool failed() const { return !error_.empty(); }

    // --- tokenizer ---------------------------------------------------------
    // Comments (`# …`) are dropped; newlines become Newline tokens (collapsed in
    // the parser), except inside (), [], {} where Python implicitly joins lines.
    // `;` is its own statement separator (an Op the parser treats like a Newline).
    void tokenize() {
        const std::string text(source_);
        const std::size_t size = text.size();

        const auto ident_start = [](char character) {
            return std::isalpha(static_cast<unsigned char>(character)) ||
                   character == '_';
        };
        const auto ident_char = [](char character) {
            return std::isalnum(static_cast<unsigned char>(character)) ||
                   character == '_';
        };

        int bracket_depth = 0;  // suppress Newline tokens while > 0 (line joining)
        std::size_t position = 0;
        while (position < size && !failed()) {
            const char character = text[position];

            if (character == '\n') {
                if (bracket_depth == 0) tokens_.push_back({Tok::Newline, "\n"});
                ++position;
                continue;
            }
            if (character == '\\' && position + 1 < size &&
                text[position + 1] == '\n') {
                position += 2;  // explicit line continuation
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(character))) {
                ++position;
                continue;
            }
            if (character == '#') {  // comment to end of line
                while (position < size && text[position] != '\n') ++position;
                continue;
            }

            // String literal, with an optional prefix (f/r/b/u, any case + combo).
            // The raw contents are stored; f-string interpolation is not evaluated.
            std::size_t prefix_end = position;
            while (prefix_end < size) {
                const char prefix = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(text[prefix_end])));
                if (prefix == 'f' || prefix == 'r' || prefix == 'b' ||
                    prefix == 'u') {
                    ++prefix_end;
                } else {
                    break;
                }
            }
            if (prefix_end < size &&
                (text[prefix_end] == '\'' || text[prefix_end] == '"') &&
                prefix_end - position <= 2) {
                if (!tokenize_string(text, position, prefix_end)) return;
                position = string_cursor_;
                continue;
            }

            if (ident_start(character)) {
                std::size_t end = position + 1;
                while (end < size && ident_char(text[end])) ++end;
                tokens_.push_back(
                    {Tok::Name, text.substr(position, end - position)});
                position = end;
                continue;
            }

            if (std::isdigit(static_cast<unsigned char>(character)) ||
                (character == '.' && position + 1 < size &&
                 std::isdigit(static_cast<unsigned char>(text[position + 1])))) {
                std::size_t end = position;
                while (end < size) {
                    const char digit = text[end];
                    if (std::isalnum(static_cast<unsigned char>(digit)) ||
                        digit == '.' || digit == '_' ||
                        ((digit == '+' || digit == '-') && end > position &&
                         (text[end - 1] == 'e' || text[end - 1] == 'E'))) {
                        ++end;
                    } else {
                        break;
                    }
                }
                tokens_.push_back(
                    {Tok::Number, text.substr(position, end - position)});
                position = end;
                continue;
            }

            if (character == '(' || character == '[' || character == '{') {
                ++bracket_depth;
                tokens_.push_back({Tok::Op, std::string(1, character)});
                ++position;
                continue;
            }
            if (character == ')' || character == ']' || character == '}') {
                if (bracket_depth > 0) --bracket_depth;
                tokens_.push_back({Tok::Op, std::string(1, character)});
                ++position;
                continue;
            }

            // Remaining single-char punctuation we navigate by. Anything else (a
            // binary operator like + or *, etc.) is also emitted as an Op so the
            // parser can reject it with a precise "unsupported" message in context.
            tokens_.push_back({Tok::Op, std::string(1, character)});
            ++position;
        }
        tokens_.push_back({Tok::End, ""});
    }

    // Scan a (possibly prefixed, possibly triple-quoted) string literal starting
    // at `prefix_end` (the quote). Pushes a String token, records where scanning
    // stopped in string_cursor_, and returns true; on an unterminated literal it
    // sets the error and returns false.
    bool tokenize_string(const std::string& text, std::size_t start,
                         std::size_t prefix_end) {
        const std::size_t size = text.size();
        const char quote = text[prefix_end];
        bool raw = false;
        for (std::size_t k = start; k < prefix_end; ++k) {
            if (std::tolower(static_cast<unsigned char>(text[k])) == 'r')
                raw = true;
        }
        std::size_t cursor = prefix_end;
        const bool triple = cursor + 2 < size && text[cursor + 1] == quote &&
                            text[cursor + 2] == quote;
        cursor += triple ? 3 : 1;
        std::string value;
        bool closed = false;
        while (cursor < size) {
            const char current = text[cursor];
            if (!raw && current == '\\' && cursor + 1 < size) {
                const char escaped = text[cursor + 1];
                switch (escaped) {
                    case 'n': value.push_back('\n'); break;
                    case 't': value.push_back('\t'); break;
                    case 'r': value.push_back('\r'); break;
                    case '\\': value.push_back('\\'); break;
                    case '\'': value.push_back('\''); break;
                    case '"': value.push_back('"'); break;
                    case '\n': break;  // line continuation inside a string
                    default:
                        value.push_back('\\');
                        value.push_back(escaped);
                        break;
                }
                cursor += 2;
                continue;
            }
            if (current == quote) {
                if (triple) {
                    if (cursor + 2 < size && text[cursor + 1] == quote &&
                        text[cursor + 2] == quote) {
                        cursor += 3;
                        closed = true;
                        break;
                    }
                    value.push_back(current);
                    ++cursor;
                    continue;
                }
                ++cursor;
                closed = true;
                break;
            }
            value.push_back(current);
            ++cursor;
        }
        if (!closed) {
            fail("unterminated string literal");
            return false;
        }
        tokens_.push_back({Tok::String, std::move(value)});
        string_cursor_ = cursor;
        return true;
    }

    // --- cursor ------------------------------------------------------------
    const Token& peek() const { return tokens_[position_]; }
    const Token& peek_next() const {
        return tokens_[position_ + 1 < tokens_.size() ? position_ + 1
                                                      : tokens_.size() - 1];
    }
    // Consume the current token (never advancing past End, so peek() stays valid).
    const Token& advance() {
        const Token& token = tokens_[position_];
        if (token.type != Tok::End) ++position_;
        return token;
    }
    bool at_end() const { return peek().type == Tok::End; }

    bool is_op(const Token& token, char character) const {
        return token.type == Tok::Op && token.text.size() == 1 &&
               token.text[0] == character;
    }
    bool accept_op(char character) {
        if (is_op(peek(), character)) {
            advance();
            return true;
        }
        return false;
    }
    void expect_op(char character) {
        if (!accept_op(character)) {
            fail(std::string("expected '") + character + "', got '" + peek().text +
                 "'");
        }
    }
    bool accept_newline_or_semicolon() {
        if (peek().type == Tok::Newline || is_op(peek(), ';')) {
            advance();
            return true;
        }
        return false;
    }

    // --- statements --------------------------------------------------------
    Statement parse_statement() {
        const Token& first = peek();
        if (first.type == Tok::Name) {
            if (first.text == "import" || first.text == "from") {
                skip_import();
                return Statement{};  // recorded as nothing — no step
            }
            if (is_unsupported_keyword(first.text)) {
                fail("unsupported Python construct '" + first.text +
                     "' (this frontend parses assignments, calls, "
                     "attribute/method access, and literals only)");
                return Statement{};
            }
            // Assignment iff `NAME =` (and not `==`); otherwise a bare expression.
            if (peek_next().type == Tok::Op && peek_next().text == "=") {
                Statement statement;
                statement.target = advance().text;  // the NAME
                advance();                           // the '='
                statement.value = parse_expr();
                return statement;
            }
        }
        Statement statement;
        statement.value = parse_expr();
        return statement;
    }

    // import a.b.c [as d]   |   from a.b import x, y   |   from a import *
    void skip_import() {
        advance();  // 'import' or 'from'
        while (!at_end() && peek().type != Tok::Newline && !is_op(peek(), ';')) {
            advance();
        }
    }

    // --- expressions -------------------------------------------------------
    // expr := atom trailer*   where a trailer is `.name`, `(args)`, or `[index]`.
    ExprPtr parse_expr() { return parse_postfix(); }

    ExprPtr parse_postfix() {
        ExprPtr expr = parse_atom();
        while (!failed()) {
            if (accept_op('.')) {
                if (peek().type != Tok::Name) {
                    fail("expected an attribute name after '.', got '" +
                         peek().text + "'");
                    return expr;
                }
                ExprPtr attribute = make_expr(ExprKind::Attribute);
                attribute->attribute = advance().text;
                attribute->base = std::move(expr);
                expr = std::move(attribute);
                continue;
            }
            if (accept_op('(')) {
                ExprPtr call = make_expr(ExprKind::Call);
                call->base = std::move(expr);
                call->args = parse_arguments();
                expect_op(')');
                expr = std::move(call);
                continue;
            }
            if (accept_op('[')) {
                ExprPtr subscript = make_expr(ExprKind::Subscript);
                subscript->base = std::move(expr);
                subscript->index = parse_expr();
                expect_op(']');
                expr = std::move(subscript);
                continue;
            }
            break;
        }
        return expr;
    }

    std::vector<Arg> parse_arguments() {
        std::vector<Arg> args;
        if (is_op(peek(), ')')) return args;  // empty call
        while (!failed()) {
            Arg arg;
            // keyword argument: NAME = value (NAME not followed by '==')
            if (peek().type == Tok::Name && peek_next().type == Tok::Op &&
                peek_next().text == "=") {
                arg.keyword = advance().text;  // NAME
                advance();                      // '='
            }
            arg.value = parse_expr();
            if (failed()) break;
            args.push_back(std::move(arg));
            if (accept_op(',')) {
                if (is_op(peek(), ')')) break;  // trailing comma
                continue;
            }
            break;
        }
        return args;
    }

    ExprPtr parse_atom() {
        const Token& token = peek();

        if (token.type == Tok::Number) {
            ExprPtr expr = make_expr(ExprKind::Literal);
            bool ok = true;
            expr->literal = parse_number(advance().text, &ok);
            if (!ok) fail("invalid number literal");
            return expr;
        }
        if (token.type == Tok::String) {
            ExprPtr expr = make_expr(ExprKind::Literal);
            expr->literal = advance().text;
            return expr;
        }
        if (token.type == Tok::Name) {
            if (token.text == "True" || token.text == "False") {
                ExprPtr expr = make_expr(ExprKind::Literal);
                expr->literal = (advance().text == "True");
                return expr;
            }
            if (token.text == "None") {
                advance();
                ExprPtr expr = make_expr(ExprKind::Literal);
                expr->literal = nullptr;
                return expr;
            }
            if (is_unsupported_keyword(token.text)) {
                fail("unsupported Python construct '" + token.text + "'");
                return nullptr;
            }
            ExprPtr expr = make_expr(ExprKind::Name);
            expr->name = advance().text;
            return expr;
        }
        if (is_op(token, '-') || is_op(token, '+')) {
            // Unary sign on a numeric literal only (no general arithmetic).
            const bool negate = advance().text == "-";
            if (peek().type != Tok::Number) {
                fail("unary sign is only supported on a number literal");
                return nullptr;
            }
            ExprPtr expr = make_expr(ExprKind::Literal);
            bool ok = true;
            nlohmann::json number = parse_number(advance().text, &ok);
            if (!ok) {
                fail("invalid number literal");
                return expr;
            }
            if (negate) {
                if (number.is_number_integer()) {
                    number = -number.get<long long>();
                } else {
                    number = -number.get<double>();
                }
            }
            expr->literal = std::move(number);
            return expr;
        }
        if (accept_op('(')) {
            ExprPtr inner = parse_expr();
            expect_op(')');
            return inner;
        }
        if (accept_op('[')) {
            ExprPtr expr = make_expr(ExprKind::List);
            if (!is_op(peek(), ']')) {
                while (!failed()) {
                    expr->items.push_back(parse_expr());
                    if (failed()) break;
                    if (accept_op(',')) {
                        if (is_op(peek(), ']')) break;  // trailing comma
                        continue;
                    }
                    break;
                }
            }
            expect_op(']');
            return expr;
        }
        if (accept_op('{')) {
            ExprPtr expr = make_expr(ExprKind::Dict);
            if (!is_op(peek(), '}')) {
                while (!failed()) {
                    ExprPtr key = parse_expr();
                    expect_op(':');
                    ExprPtr value = parse_expr();
                    if (failed()) break;
                    expr->entries.emplace_back(std::move(key), std::move(value));
                    if (accept_op(',')) {
                        if (is_op(peek(), '}')) break;  // trailing comma
                        continue;
                    }
                    break;
                }
            }
            expect_op('}');
            return expr;
        }

        fail("unexpected '" + token.text + "' (expected a value, call, or name)");
        return nullptr;
    }

    std::string_view source_;
    std::vector<Token> tokens_;
    std::size_t position_ = 0;
    std::size_t string_cursor_ = 0;  // where tokenize_string stopped
    std::string error_;
};

// ===========================================================================
// Lowering — walk the AST, build the ExecutionGraph IR (never fails)
// ===========================================================================

// The value an expression evaluates to, for the lowering pass: the single
// "result" node it produces (for chaining + assignment binding) and the set of
// producer nodes its value draws on (for the data-flow edges into a consuming
// call). A literal has neither; a call has itself as both.
struct Lowered {
    std::optional<NodeId> value;    // the canonical producer (for binding)
    std::vector<NodeId> producers;  // every node this value depends on
};

class Lowerer {
public:
    ExecutionGraph run(const std::vector<Statement>& statements) {
        for (const Statement& statement : statements) {
            if (!statement.value) continue;
            Lowered lowered = lower(*statement.value);
            if (!statement.target.empty()) {
                bindings_[statement.target] = lowered.value;
                // Tag the produced node with the variable it was assigned to —
                // provenance only, for traceability in the workspace.
                if (lowered.value && statement.value->kind == ExprKind::Call) {
                    if (auto node = graph_.find_node(*lowered.value)) {
                        node->attributes["python"]["assigned_to"] =
                            statement.target;
                    }
                }
            }
        }
        return std::move(graph_);
    }

private:
    Lowered lower(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::Literal:
                return {};
            case ExprKind::Name: {
                auto found = bindings_.find(expr.name);
                if (found != bindings_.end() && found->second) {
                    return {found->second, {*found->second}};
                }
                return {};  // a free / unbound name carries no node
            }
            case ExprKind::Attribute:
                // Plain attribute access (no call) is transparent: its value is
                // its base's value.
                return lower(*expr.base);
            case ExprKind::Subscript: {
                Lowered base = lower(*expr.base);
                Lowered index = lower(*expr.index);
                append(base.producers, index.producers);
                return base;  // a subscript's value flows from its base
            }
            case ExprKind::List: {
                Lowered result;
                for (const ExprPtr& item : expr.items) {
                    Lowered element = lower(*item);
                    append(result.producers, element.producers);
                }
                return result;  // a list is not itself a single producer
            }
            case ExprKind::Dict: {
                Lowered result;
                for (const auto& [key, value] : expr.entries) {
                    Lowered key_lowered = lower(*key);
                    Lowered value_lowered = lower(*value);
                    append(result.producers, key_lowered.producers);
                    append(result.producers, value_lowered.producers);
                }
                return result;
            }
            case ExprKind::Call:
                return lower_call(expr);
        }
        return {};
    }

    Lowered lower_call(const Expr& call) {
        std::vector<NodeId> inputs;
        std::string verb;

        const Expr& callee = *call.base;
        if (callee.kind == ExprKind::Attribute) {
            // Method call: the receiver feeds the method node.
            Lowered receiver = lower(*callee.base);
            append(inputs, receiver.producers);
            verb = callee.attribute;
        } else if (callee.kind == ExprKind::Name) {
            verb = callee.name;
        } else {
            // Calling the result of another expression, e.g. (factory())(x): the
            // callee's producers feed this node; it has no nameable verb.
            Lowered callee_lowered = lower(callee);
            append(inputs, callee_lowered.producers);
            verb = "call";
        }

        // Every argument's producers become data-flow inputs to this step.
        nlohmann::json positional = nlohmann::json::array();
        nlohmann::json keywords = nlohmann::json::object();
        for (const Arg& arg : call.args) {
            Lowered argument = lower(*arg.value);
            append(inputs, argument.producers);
            const nlohmann::json captured = capture_argument(*arg.value);
            if (arg.keyword.empty()) {
                positional.push_back(captured);
            } else {
                keywords[arg.keyword] = captured;
            }
        }

        const NodeId id = fresh_id(verb);
        const StepKind kind = python_step_kind(verb).value_or(StepKind::Clean);
        nlohmann::json provenance{{"call", verb}};
        if (!positional.empty()) provenance["args"] = std::move(positional);
        if (!keywords.empty()) provenance["kwargs"] = std::move(keywords);
        graph_.add_node(kind, verb,
                        nlohmann::json{{"python", std::move(provenance)}}, id);

        for (const NodeId& input : inputs) {
            graph_.add_edge(input, id, EdgeKind::DataFlow);  // dedupes internally
        }
        return {id, {id}};
    }

    // A compact, JSON-able record of a call argument for the `python` provenance
    // bag: a literal keeps its value; anything else (a variable, a nested call, a
    // collection) is recorded as a reference marker so the bag stays serializable
    // without trying to capture runtime values.
    static nlohmann::json capture_argument(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::Literal:
                return expr.literal;
            case ExprKind::Name:
                return nlohmann::json{{"ref", expr.name}};
            case ExprKind::Call:
                if (expr.base && expr.base->kind == ExprKind::Name) {
                    return nlohmann::json{{"call", expr.base->name}};
                }
                if (expr.base && expr.base->kind == ExprKind::Attribute) {
                    return nlohmann::json{{"call", expr.base->attribute}};
                }
                return nlohmann::json{{"call", nullptr}};
            case ExprKind::List: {
                nlohmann::json out = nlohmann::json::array();
                for (const ExprPtr& item : expr.items) {
                    out.push_back(capture_argument(*item));
                }
                return out;
            }
            case ExprKind::Attribute:
                return nlohmann::json{{"ref", expr.attribute}};
            default:
                return nullptr;
        }
    }

    // A deterministic, unique node id derived from the verb: the verb itself for
    // its first use, then `verb_2`, `verb_3`, … for repeats — stable across runs
    // (numbered in source order), so the same script always yields the same ids.
    NodeId fresh_id(const std::string& verb) {
        const std::string base = verb.empty() ? "step" : verb;
        int& count = verb_counts_[base];
        ++count;
        NodeId candidate = count == 1 ? base : base + "_" + std::to_string(count);
        while (graph_.has_node(candidate)) {
            ++count;
            candidate = base + "_" + std::to_string(count);
        }
        return candidate;
    }

    static void append(std::vector<NodeId>& into,
                       const std::vector<NodeId>& from) {
        into.insert(into.end(), from.begin(), from.end());
    }

    ExecutionGraph graph_;
    std::unordered_map<std::string, std::optional<NodeId>> bindings_;
    std::unordered_map<std::string, int> verb_counts_;
};

// The non-throwing entry: parse (Parser::run records any error) then lower.
FrontendResult compile_source(std::string_view source) {
    FrontendResult result;
    ParsedProgram parsed = Parser(source).run();
    if (!parsed.error.empty()) {
        result.error = parsed.error;
        return result;
    }
    Lowerer lowerer;
    result.graph = lowerer.run(parsed.statements);
    if (result.graph.empty()) {
        result.error =
            "no pipeline steps recognized (the script has no function calls to "
            "lower into graph steps)";
    }
    return result;
}

}  // namespace

// The verb → StepKind table. Keyed on the bare function/method name. This is the
// documented extension point as the use-case catalog grows; an unmapped verb is a
// hard miss here (std::nullopt) — PythonFrontend then defaults it to Clean, but
// the lookup stays honest so the recognized vocabulary is visible + testable.
std::optional<ir::StepKind> python_step_kind(std::string_view verb) {
    static const std::unordered_map<std::string_view, StepKind> table = {
        // connect — pull data in / kick the pipeline off
        {"load_dataset", StepKind::Connect}, {"load", StepKind::Connect},
        {"read_csv", StepKind::Connect},     {"read_sql", StepKind::Connect},
        {"read_json", StepKind::Connect},    {"read_parquet", StepKind::Connect},
        {"read_table", StepKind::Connect},   {"connect", StepKind::Connect},
        {"source", StepKind::Connect},       {"fetch", StepKind::Connect},
        {"extract", StepKind::Connect},      {"open", StepKind::Connect},
        // clean — normalize / reshape / compute features
        {"clean", StepKind::Clean},          {"dropna", StepKind::Clean},
        {"fillna", StepKind::Clean},         {"normalize", StepKind::Clean},
        {"dedupe", StepKind::Clean},         {"transform", StepKind::Clean},
        {"featurize", StepKind::Clean},      {"features", StepKind::Clean},
        {"prepare", StepKind::Clean},        {"preprocess", StepKind::Clean},
        {"filter", StepKind::Clean},         {"select", StepKind::Clean},
        {"join", StepKind::Clean},           {"merge", StepKind::Clean},
        {"groupby", StepKind::Clean},        {"aggregate", StepKind::Clean},
        {"encode", StepKind::Clean},         {"scale", StepKind::Clean},
        {"impute", StepKind::Clean},
        // model — train / evaluate / calibrate a model (the native GBDT trainer the
        // ChurnStepExecutor runs in the model step). Applying a trained model to
        // rank entities is the index stage below — not this one.
        {"train", StepKind::Model},          {"fit", StepKind::Model},
        {"evaluate", StepKind::Model},       {"model", StepKind::Model},
        {"classify", StepKind::Model},       {"cluster", StepKind::Model},
        {"forecast", StepKind::Model},       {"calibrate", StepKind::Model},
        // index — build a search / vector / feature index, and batch-score / rank
        // entities through a trained model. `score`/`predict`/`rank` are inference,
        // not training: they lower to the index step the ChurnStepExecutor runs the
        // score job (the at-risk list) in.
        {"index", StepKind::Index},          {"build_index", StepKind::Index},
        {"embed", StepKind::Index},          {"vectorize", StepKind::Index},
        {"upsert", StepKind::Index},         {"rank", StepKind::Index},
        {"score", StepKind::Index},          {"predict", StepKind::Index},
        // serve — expose results behind a query / API / endpoint, incl. per-entity
        // explanation. `explain` is TreeSHAP — the explain job the ChurnStepExecutor
        // runs in the serve step (the churn drivers), not a model-training step.
        {"serve", StepKind::Serve},          {"deploy", StepKind::Serve},
        {"endpoint", StepKind::Serve},       {"expose", StepKind::Serve},
        {"publish", StepKind::Serve},        {"explain", StepKind::Serve},
        // act — take an action on the world, incl. cluster control. `add_nodes` is
        // the permanent-add control action (zetta::cloud::add_nodes): provisioning nodes is
        // a side effect on the cluster, so it lowers to the act stage.
        {"notify", StepKind::Act},           {"alert", StepKind::Act},
        {"send", StepKind::Act},             {"act", StepKind::Act},
        {"write", StepKind::Act},            {"email", StepKind::Act},
        {"slack", StepKind::Act},            {"webhook", StepKind::Act},
        {"trigger", StepKind::Act},          {"export", StepKind::Act},
        {"save", StepKind::Act},             {"add_nodes", StepKind::Act},
        {"add_node", StepKind::Act},
    };
    const auto found = table.find(verb);
    if (found == table.end()) return std::nullopt;
    return found->second;
}

FrontendResult PythonFrontend::compile(std::string_view source) const {
    return compile_source(source);
}

}  // namespace zetta::input
