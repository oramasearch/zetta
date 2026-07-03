#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Crash reports (docs/architecture/observability.md, #640): a fatal signal
// (SIGSEGV / SIGABRT / SIGBUS / SIGILL / SIGFPE) writes one JSON report —
// stacktrace, recent spans, version, platform — to
// `<data_dir>/crashes/<uuid>.json` before the process dies, so the next start
// (and the `system.crashes` read view) can surface WHY the last run ended.
//
// The handler is best-effort by design: a fatal-signal context cannot safely
// allocate or lock, so everything that can be captured ahead of time (the
// report id, the destination path, version/platform) is captured at install
// time, the span ring is read with a try-lock (never a wait), and the signal
// disposition is reset to SIG_DFL first so a second fault kills the process
// instead of looping. Losing a report to a hostile crash is acceptable;
// hanging the dying process is not.
namespace zetta::observability {
class SpanRecorder;

// One parsed crash report (the shape of `crashes/<uuid>.json`).
struct CrashReport {
    std::string id;           // the report file's uuid stem
    int signal_number = 0;
    std::string signal_name;  // "SIGSEGV", ...
    std::string crashed_at;   // ISO-8601 UTC
    std::string version;      // zetta::kVersion at crash time
    std::string platform;     // "macos-arm64", "linux-x86_64", ...
    std::vector<std::string> stacktrace;   // one frame per entry
    nlohmann::json recent_spans = nlohmann::json::array();  // span to_json rows
};

nlohmann::json to_json(const CrashReport& report);

// Schema-as-code for the report file (draft-07; generated mirror
// schemas/crash_report.schema.json). Reports are validated when read back —
// a malformed file surfaces its reason instead of corrupting the view.
const nlohmann::json& crash_report_schema();
std::optional<CrashReport> parse_crash_report(const nlohmann::json& value,
                                              std::string* error = nullptr);

struct CrashHandlerOptions {
    std::string crashes_dir;  // usually <data_dir>/crashes; created on install
    // Optional span ring to snapshot into the report (read with a try-lock).
    const SpanRecorder* recorder = nullptr;
};

// Install the fatal-signal handler. Returns false (with a reason) when the
// crashes directory cannot be created. Reinstalling replaces the options
// (process-wide state — one crash handler per process, last install wins).
bool install_crash_handler(CrashHandlerOptions options,
                           std::string* error = nullptr);

// One report file read back, valid or not: `report` is set when the file
// parsed + validated; otherwise `error` carries the path-qualified reason.
// Malformed files stay observable instead of silently vanishing.
struct LoadedCrashReport {
    std::string path;
    std::optional<CrashReport> report;
    std::string error;
};

// Every `*.json` under `crashes_dir`, sorted oldest first by filename mtime
// order falling back to name. Missing directory = no crashes = empty.
std::vector<LoadedCrashReport> load_crash_reports(
    const std::string& crashes_dir);

}  // namespace zetta::observability
