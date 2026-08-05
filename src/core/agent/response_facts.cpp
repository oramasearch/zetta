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
        if (!is_number_char(text[index])) {
            ++index;
            continue;
        }
        const std::size_t begin = index;
        while (index < text.size() && is_number_char(text[index])) ++index;
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
    EndState state = graph_solution_facts(action.execution_graph);

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
        if (!contains(grounding, literal)) {
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
