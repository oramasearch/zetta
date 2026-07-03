#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The REPL console model (`zetta::repl`) — the engine-agnostic core behind the
// CMP-6 REPL console (#524). It holds a REPL *session*: the selected frontend,
// the input buffer, and the scrollback transcript, plus the submit logic that
// runs the current input through a host-supplied handler and records the
// outcome. It carries **no UI dependency** (no SDL/ImGui) and lives in
// zetta-core, so the same class backs the headed `zetta::gui::ReplConsole`
// component today and a future CLI REPL (each is a thin renderer over this
// model — the testkit pattern). See docs/gui/repl-console.md.
//
// The model owns no engine: running a statement is a *callback* (the "run
// event" the console emits). The host wires the handler to the real engine —
// SQL through `query::QueryService`, the simulated-Python surface through the
// frontend→IR lowering layer when it lands (ADR-33 / ARC-15) — and the model
// only collects input and stores the result. That keeps the component reusable
// and decoupled from any single frontend.
namespace zetta::repl {

// The language a REPL statement is written in. Each frontend lowers to the same
// ExecutionGraph IR core; the selector only tells the run handler how to
// interpret the source text. SQL is wired today; simulated-Python is an honest
// seam until the lowering layer exists.
enum class Frontend { Sql, SimPython };

// The canonical lowercase id ("sql" / "sim-python") and a human label
// ("SQL" / "Python (simulated)") for the selector + transcript.
std::string_view to_string(Frontend frontend);
std::string_view frontend_label(Frontend frontend);

// All frontends, in selector order — the combo's option list.
const std::vector<Frontend>& frontends();

// The result of running one statement, frontend-agnostic so one transcript
// holds SQL rows and simulated-Python output alike. `columns` + `rows` populate
// the result grid (each row is a column-aligned vector of stringified cells);
// `messages` are status lines (one per executed statement); a non-empty `error`
// marks a failed run and is rendered inline (red in the GUI). `elapsed` is a
// pre-formatted wall-clock string the handler may fill (the handler times the
// engine, since only it knows what work ran).
struct Result {
    std::vector<std::string> messages;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::string error;
    std::string elapsed;

    bool ok() const { return error.empty(); }

    static Result failure(std::string reason) {
        Result result;
        result.error = std::move(reason);
        return result;
    }
};

// One scrollback entry: the frontend + exact source that was run, and the result
// it produced. The transcript is an ordered list of these (oldest first).
struct Entry {
    Frontend frontend = Frontend::Sql;
    std::string source;
    Result result;
};

// The host-supplied run handler — the "run event" the console emits. Given the
// selected frontend + source text, it executes the statement and returns a
// Result. The model invokes it from submit(); it never runs an engine itself.
using RunHandler = std::function<Result(Frontend frontend, const std::string& source)>;

// A REPL session's interactive state + behavior, with no UI dependency. The GUI
// component (and a future CLI) bind their widgets to it and render its
// transcript.
class ConsoleModel {
public:
    Frontend frontend() const { return frontend_; }
    void set_frontend(Frontend frontend) { frontend_ = frontend; }

    // The input buffer. Mutable so an editor (ImGui InputTextMultiline, a CLI
    // line read) can bind directly to it; submit() reads it.
    std::string& input() { return input_; }
    const std::string& input() const { return input_; }

    const std::vector<Entry>& transcript() const { return transcript_; }
    bool empty() const { return transcript_.empty(); }
    std::size_t size() const { return transcript_.size(); }

    // Run the current input through `run` and append the (frontend, source,
    // result) to the transcript. Returns true when an entry was recorded.
    //
    // A blank / whitespace-only input or a null `run` is a no-op (returns false,
    // records nothing) — mirroring `Project::capture_intent` rejecting empty
    // intent. Both a successful and a failed (error) result are recorded, so the
    // transcript shows errors inline; only the recorded-an-entry case clears the
    // input buffer (the prompt resets for the next statement, REPL-style).
    bool submit(const RunHandler& run);

    // Drop the whole transcript (the Clear control). Leaves the input + the
    // selected frontend untouched.
    void clear() { transcript_.clear(); }

private:
    Frontend frontend_ = Frontend::Sql;
    std::string input_;
    std::vector<Entry> transcript_;
};

// Render a Result as plain text — status messages, an inline error, and a
// column-aligned table of the result grid. The reusable text projection a CLI
// REPL prints (the GUI draws the same Result as an ImGui table instead); also
// the headless-testable form of the grid. Returns "" for an empty, message-less
// result.
std::string format_result(const Result& result);

// Render the whole transcript as plain text — each entry as a prompt line
// (`frontend> source`) followed by its formatted result. The CLI's scrollback.
std::string format_transcript(const ConsoleModel& model);

}  // namespace zetta::repl
