#include "zetta/observability/crash.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

#include <nlohmann/json-schema.hpp>

#include "zetta/observability/trace.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"
#include "zetta/version.hpp"

namespace zetta::observability {

namespace {

// Everything the fatal-signal handler needs, captured at install time — a
// signal context must not generate uuids, query the OS, or take locks it can
// avoid. Process-wide by nature (there is one signal disposition table);
// written only by install_crash_handler, read by the dying handler.
struct CrashHandlerState {
    std::string report_path;  // <crashes_dir>/<uuid>.json, pre-generated
    std::string report_id;
    std::string version;
    std::string platform;
    const SpanRecorder* recorder = nullptr;
};

CrashHandlerState& handler_state() {
    static CrashHandlerState state;
    return state;
}

constexpr int kFatalSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};

std::string signal_name_of(int signal_number) {
    switch (signal_number) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        default:      return "SIG" + std::to_string(signal_number);
    }
}

// The actual report write, called from the signal handler. Best-effort: the
// JSON build allocates (not strictly async-signal-safe), but the disposition
// has already been reset to SIG_DFL, so a fault inside THIS function kills
// the process instead of recursing — the accepted trade for getting a
// stacktrace + spans out of a dying node.
void write_crash_report(int signal_number) {
    const CrashHandlerState& state = handler_state();
    if (state.report_path.empty()) return;

    void* frames[64];
    const int frame_count = backtrace(frames, 64);

    CrashReport report;
    report.id = state.report_id;
    report.signal_number = signal_number;
    report.signal_name = signal_name_of(signal_number);
    report.crashed_at = zetta::util::now_iso8601();
    report.version = state.version;
    report.platform = state.platform;
    if (char** symbols = backtrace_symbols(frames, frame_count)) {
        for (int frame = 0; frame < frame_count; ++frame) {
            report.stacktrace.emplace_back(symbols[frame]);
        }
        std::free(symbols);
    }
    if (state.recorder) {
        for (const Span& span : state.recorder->try_recent(32)) {
            report.recent_spans.push_back(to_json(span));
        }
    }

    const std::string body = to_json(report).dump(2);
    const int file_descriptor = ::open(state.report_path.c_str(),
                                       O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (file_descriptor >= 0) {
        ssize_t written = 0;
        while (written < static_cast<ssize_t>(body.size())) {
            const ssize_t chunk =
                ::write(file_descriptor, body.data() + written,
                        body.size() - static_cast<std::size_t>(written));
            if (chunk <= 0) break;
            written += chunk;
        }
        ::close(file_descriptor);
    }
}

void on_fatal_signal(int signal_number) {
    // First thing: restore the default disposition, so a second fault (or the
    // re-raise below) terminates the process for real — never a handler loop.
    std::signal(signal_number, SIG_DFL);
    write_crash_report(signal_number);
    std::raise(signal_number);
}

const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.CrashReport",
      "type": "object",
      "properties": {
        "id":            { "type": "string" },
        "signal_number": { "type": "integer" },
        "signal_name":   { "type": "string" },
        "crashed_at":    { "type": "string" },
        "version":       { "type": "string" },
        "platform":      { "type": "string" },
        "stacktrace":    { "type": "array", "items": { "type": "string" } },
        "recent_spans":  { "type": "array", "items": { "type": "object" } }
      },
      "required": ["id", "signal_number", "signal_name", "crashed_at",
                   "version", "platform", "stacktrace"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// First-failure collector (the run.cpp pattern): keeps validation failures
// from crossing the validator-library boundary as exceptions.
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

}  // namespace

std::string platform_string() {
#if defined(__APPLE__)
    const char* operating_system = "macos";
#elif defined(__linux__)
    const char* operating_system = "linux";
#else
    const char* operating_system = "unknown";
#endif
#if defined(__aarch64__) || defined(__arm64__)
    const char* architecture = "arm64";
#elif defined(__x86_64__)
    const char* architecture = "x86_64";
#else
    const char* architecture = "unknown";
#endif
    return std::string(operating_system) + "-" + architecture;
}

nlohmann::json to_json(const CrashReport& report) {
    return nlohmann::json{{"id", report.id},
                          {"signal_number", report.signal_number},
                          {"signal_name", report.signal_name},
                          {"crashed_at", report.crashed_at},
                          {"version", report.version},
                          {"platform", report.platform},
                          {"stacktrace", report.stacktrace},
                          {"recent_spans", report.recent_spans}};
}

const nlohmann::json& crash_report_schema() { return schema_document(); }

std::optional<CrashReport> parse_crash_report(const nlohmann::json& value,
                                              std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return std::nullopt;
    }
    CrashReport report;
    report.id = value.at("id").get<std::string>();
    report.signal_number = value.at("signal_number").get<int>();
    report.signal_name = value.at("signal_name").get<std::string>();
    report.crashed_at = value.at("crashed_at").get<std::string>();
    report.version = value.at("version").get<std::string>();
    report.platform = value.at("platform").get<std::string>();
    report.stacktrace =
        value.at("stacktrace").get<std::vector<std::string>>();
    report.recent_spans = value.value("recent_spans", nlohmann::json::array());
    return report;
}

bool install_crash_handler(CrashHandlerOptions options, std::string* error) {
    std::error_code directory_error;
    std::filesystem::create_directories(options.crashes_dir, directory_error);
    if (directory_error) {
        if (error) {
            *error = "crashes dir \"" + options.crashes_dir +
                     "\": " + directory_error.message();
        }
        return false;
    }

    CrashHandlerState& state = handler_state();
    state.report_id = zetta::util::new_uuid();
    state.report_path = (std::filesystem::path(options.crashes_dir) /
                         (state.report_id + ".json"))
                            .string();
    state.version = zetta::kVersion;
    state.platform = platform_string();
    state.recorder = options.recorder;

    for (const int fatal_signal : kFatalSignals) {
        std::signal(fatal_signal, on_fatal_signal);
    }
    return true;
}

std::vector<LoadedCrashReport> load_crash_reports(
    const std::string& crashes_dir) {
    std::vector<LoadedCrashReport> loaded;
    std::error_code directory_error;
    std::filesystem::directory_iterator entries(crashes_dir, directory_error);
    if (directory_error) return loaded;  // no directory = no crashes

    std::vector<std::filesystem::path> report_files;
    for (const auto& entry : entries) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            report_files.push_back(entry.path());
        }
    }
    std::sort(report_files.begin(), report_files.end(),
              [](const std::filesystem::path& left,
                 const std::filesystem::path& right) {
                  std::error_code time_error;
                  const auto left_time =
                      std::filesystem::last_write_time(left, time_error);
                  const auto right_time =
                      std::filesystem::last_write_time(right, time_error);
                  if (left_time != right_time) return left_time < right_time;
                  return left < right;  // oldest first, name as tiebreak
              });

    for (const std::filesystem::path& report_file : report_files) {
        LoadedCrashReport entry;
        entry.path = report_file.string();
        std::ifstream input(report_file);
        const nlohmann::json value = nlohmann::json::parse(
            input, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (value.is_discarded()) {
            entry.error = report_file.filename().string() + ": not valid JSON";
        } else {
            std::string reason;
            entry.report = parse_crash_report(value, &reason);
            if (!entry.report) {
                entry.error = report_file.filename().string() + ": " + reason;
            }
        }
        loaded.push_back(std::move(entry));
    }
    return loaded;
}

}  // namespace zetta::observability
