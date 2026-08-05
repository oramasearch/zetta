#include "zetta/agent/response_facts.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/agent/solution_graph.hpp"

namespace zetta::agent {
namespace {

bool is_number_char(char character) {
    return (character >= '0' && character <= '9') || character == '.';
}

// Every numeric literal in `text`, as written. Comparing the LITERAL (not a parsed double) is
// deliberate: the context states "AUC 0.880", so an answer saying "0.88" has rounded a retrieved
// figure into one that was never retrieved. For a product whose pitch is verified correctness,
// that is a real defect, not a nitpick.
std::vector<std::string> numeric_literals(std::string_view text) {
    std::vector<std::string> found;
    std::size_t index = 0;
    while (index < text.size()) {
        // Skip backticked code spans wholesale. Digits inside one are part of an IDENTIFIER —
        // a column like `precision_at_10`, a step like `top_20_accounts` — not a figure the
        // answer is claiming. Counting them made a correct reply naming a real column read as a
        // fabrication, which is the opposite of what this check is for.
        if (text[index] == '`') {
            const std::size_t close = text.find('`', index + 1);
            if (close == std::string_view::npos) {
                ++index;  // a lone backtick is prose; keep checking the figures after it
                continue;
            }
            index = close + 1;
            continue;
        }
        if (!is_number_char(text[index])) {
            ++index;
            continue;
        }
        const std::size_t begin = index;
        while (index < text.size() && is_number_char(text[index])) ++index;
        // A digit run touching a letter is part of an IDENTIFIER — BM25F, GPT-4, S-2, gpt4o — not
        // a figure the answer is claiming. Same reasoning as the code-span skip above: this check
        // exists to catch invented MEASURES, and flagging an algorithm's name as a fabricated
        // number inverts it.
        const bool letter_before = begin > 0 && (std::isalpha(static_cast<unsigned char>(
                                                     text[begin - 1])) != 0);
        const bool letter_after =
            index < text.size() && (std::isalpha(static_cast<unsigned char>(text[index])) != 0);
        if (letter_before || letter_after) continue;
        std::string literal(text.substr(begin, index - begin));
        // Trim a trailing '.' that was sentence punctuation rather than a decimal point.
        while (!literal.empty() && literal.back() == '.') literal.pop_back();
        if (!literal.empty() && literal != ".") found.push_back(std::move(literal));
    }
    return found;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return !needle.empty() && haystack.find(needle) != std::string_view::npos;
}

// Does `haystack` state `literal` as a WHOLE number — not merely contain its digits?
//
// Plain substring matching silently defeated the rounding rule this file exists to enforce: the
// context states "AUC: 0.880", and "0.88" is a substring of it, so a rounded figure the engine
// never retrieved was accepted as grounded. "120" inside "1200" is the same failure. Requiring the
// neighbouring characters not to be part of a number makes the comparison what it always claimed
// to be — as written, exactly.
bool states_literal(std::string_view haystack, std::string_view literal) {
    if (literal.empty()) return false;
    const auto is_digit = [](char character) { return character >= '0' && character <= '9'; };
    for (std::size_t at = haystack.find(literal); at != std::string_view::npos;
         at = haystack.find(literal, at + 1)) {
        // The number continues to the left only through a digit, or through a decimal point that
        // itself follows a digit. A bare '.' is sentence punctuation: "accuracy 0.660." states
        // 0.660, not 0.660-followed-by-more-number.
        bool left_open = true;
        if (at >= 1) {
            if (is_digit(haystack[at - 1])) left_open = false;
            else if (haystack[at - 1] == '.' && at >= 2 && is_digit(haystack[at - 2]))
                left_open = false;
        }
        bool right_open = true;
        const std::size_t after = at + literal.size();
        if (after < haystack.size()) {
            if (is_digit(haystack[after])) right_open = false;
            else if (haystack[after] == '.' && after + 1 < haystack.size() &&
                     is_digit(haystack[after + 1]))
                right_open = false;
        }
        if (left_open && right_open) return true;
    }
    return false;
}

// The value following `label` on its own line in the context, trimmed. Empty when absent.
std::string field_after(std::string_view grounding, std::string_view label) {
    const std::size_t at = grounding.find(label);
    if (at == std::string_view::npos) return std::string();
    const std::size_t begin = at + label.size();
    const std::size_t end = grounding.find('\n', begin);
    std::string_view value =
        grounding.substr(begin, end == std::string_view::npos ? std::string_view::npos
                                                              : end - begin);
    while (!value.empty() && (value.front() == ' ' || value.front() == ':')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r' || value.back() == '.')) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

// The first indented "  name: value" entry under `section`, which is how the grounding renders a
// ranked list — so this recovers the TOP driver, the one rank fidelity depends on.
std::string first_ranked_name(std::string_view grounding, std::string_view section) {
    const std::size_t at = grounding.find(section);
    if (at == std::string_view::npos) return std::string();
    std::size_t line = grounding.find('\n', at);
    if (line == std::string_view::npos) return std::string();
    ++line;
    const std::size_t end = grounding.find('\n', line);
    std::string_view entry =
        grounding.substr(line, end == std::string_view::npos ? std::string_view::npos : end - line);
    while (!entry.empty() && entry.front() == ' ') entry.remove_prefix(1);
    // Ranked lists render either "name: 87.9%" or "1. ACCOUNT-1042 - risk 0.881".
    const std::size_t rank_dot = entry.find(". ");
    if (rank_dot != std::string_view::npos && rank_dot <= 2) entry.remove_prefix(rank_dot + 2);
    // " - " before ':' — an account name legitimately contains a hyphen ("ACME-1042"), so
    // splitting on a bare '-' would truncate the very entity we are trying to recover.
    std::size_t stop = entry.find(" - ");
    if (stop == std::string_view::npos) stop = entry.find(':');
    if (stop != std::string_view::npos) entry = entry.substr(0, stop);
    while (!entry.empty() && entry.back() == ' ') entry.remove_suffix(1);
    return std::string(entry);
}

}  // namespace

EndState response_solution_facts(const StudioAction& action, const std::string& grounding) {
    // Start from the pipeline's own facts so an `action` turn grades exactly as it always has.
    //
    // A MODEL's response carries tool calls and no graph — the agent is a tool-calling harness, so
    // the model decides which steps to call and the harness builds the Pipeline by running them.
    // Reading only `execution_graph` therefore graded every real response as having built nothing:
    // a checkpoint emitting correct calls on 100% of action cases scored 47%, with `connect` and
    // `serve` reported missing from responses that plainly called them. Build the pipeline the same
    // way the harness does when the model supplied no graph, so grading measures what the turn
    // actually produces. A response that carries a graph is unchanged.
    EndState state = graph_solution_facts(action.execution_graph.nodes().empty() &&
                                                  !action.tool_calls.empty()
                                              ? build_pipeline_from_tool_calls(action.tool_calls)
                                              : action.execution_graph);

    state.set("kind", std::string(to_string(action.kind)));
    const bool built_nothing =
        action.tool_calls.empty() && action.execution_graph.nodes().empty();
    state.set("built_nothing", built_nothing);

    // Exactly one question mark — a clarifying turn asks ONE thing. Asking three is a different
    // (worse) behavior and must not score as success.
    int question_marks = 0;
    for (const char character : action.message) {
        if (character == '?') ++question_marks;
    }
    state.set("asked_one_question", question_marks == 1);

    if (action.kind == InteractionKind::Abstain) {
        state.set("abstain", true);
    } else if (action.kind == InteractionKind::Clarify) {
        state.set("clarify", true);
    }

    // What the response asked the engine to DO is checkable directly — a governed change is
    // graded on the capability it requested, not on prose describing it.
    for (const ToolCall& call : action.tool_calls) {
        state.set("called_" + call.tool, true);
    }

    if (grounding.empty()) return state;

    // A capability answer is grounded in the registry listing the harness supplied: the fact is
    // emitted only when the message names a capability that the listing actually contains, so
    // inventing a plausible-sounding capability fails.
    if (const std::size_t listing = grounding.find("Capabilities:"); listing != std::string::npos) {
        std::size_t line = grounding.find('\n', listing);
        while (line != std::string::npos) {
            ++line;
            const std::size_t end = grounding.find('\n', line);
            std::string entry = grounding.substr(
                line, end == std::string::npos ? std::string::npos : end - line);
            while (!entry.empty() && entry.front() == ' ') entry.erase(entry.begin());
            while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\r')) entry.pop_back();
            if (entry.empty()) break;
            if (contains(action.message, entry)) {
                state.set("capability_named", entry);
                state.set("capability_exists", true);
                break;
            }
            line = end;
        }
    }

    // --- the anti-fabrication check ---------------------------------------------------------
    // Every number the answer states must be one the engine actually retrieved.
    bool numbers_from_context = true;
    for (const std::string& literal : numeric_literals(action.message)) {
        if (literal.size() < 2) continue;  // a bare digit is prose ("one of 3 drivers"), not a claim
        if (!states_literal(grounding, literal)) {
            numbers_from_context = false;
            break;
        }
    }
    state.set("numbers_from_context", numbers_from_context);
    state.set("grounded", numbers_from_context && !action.message.empty());

    // --- entity + value grounding -------------------------------------------------------------
    // Each fact is emitted ONLY when the message actually carries it, so a task requiring
    // `top_driver` is satisfied by naming the real top driver and by nothing else.
    const std::string top_driver = first_ranked_name(grounding, "Drivers, strongest first");
    if (!top_driver.empty() && contains(action.message, top_driver)) {
        state.set("top_driver", top_driver);
    }
    const std::string top_account = first_ranked_name(grounding, "Accounts most at risk");
    if (!top_account.empty() && contains(action.message, top_account)) {
        state.set("top_account", top_account);
    }

    const std::string measure = field_after(grounding, "Measure: AUC");
    if (!measure.empty()) {
        const std::string figure = measure.substr(0, measure.find(','));
        if (contains(action.message, figure)) {
            state.set("measure_auc", std::stod(figure));
        }
    }

    const std::string table = field_after(grounding, "Table:");
    if (!table.empty() && contains(action.message, table)) state.set("table", table);

    const std::string rows = field_after(grounding, "Rows:");
    if (!rows.empty()) {
        const std::string figure = rows.substr(0, rows.find(','));
        if (contains(action.message, figure)) {
            state.set("row_count", std::stoll(figure));
        }
    }

    const std::string nodes = field_after(grounding, "Cluster nodes:");
    if (!nodes.empty() && contains(action.message, nodes)) state.set("node_count", std::stoi(nodes));

    const std::string jobs = field_after(grounding, "Running jobs:");
    if (!jobs.empty() && contains(action.message, jobs)) {
        state.set("running_jobs", std::stoi(jobs));
    }

    const std::string status = field_after(grounding, "Last run:");
    if (!status.empty() && contains(action.message, status)) state.set("run_status", status);

    const std::string failure = field_after(grounding, "Failure reason:");
    if (!failure.empty() && contains(action.message, failure)) {
        state.set("failure_reason", failure);
    }

    return state;
}

}  // namespace zetta::agent
