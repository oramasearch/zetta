#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/cloud_provider.hpp"
#include "zetta/cloud/command_runner.hpp"

namespace zetta {

// The outcome of one executed inspect command — the read-only identity/inventory
// call a provider's CLI made. `output` is the captured stdout/stderr (truncated
// for display); `error` carries a reason when the command failed to run (e.g. the
// CLI isn't installed) or when its JSON couldn't be parsed/validated.
struct CloudInspectCommand {
    std::string display;    // the human form (from CloudProvider::inspect_commands)
    int exit_code = 0;      // the process exit code (127 ≈ CLI not installed)
    bool ok = false;        // the command ran and exited 0
    std::string output;     // captured output, truncated
    std::string error;      // a failure reason, if any
};

// The result of one **consent-gated, read-only** inspect of a cloud provider:
// the provider's identity (account / user / project / namespace, provider-
// specific), the locations (regions) it offers, any resources it surfaced (k8s
// nodes), and the per-command outcomes. This is zetta's own contract — it is
// cached on the Node and surfaced as `system.zetta_cloud_status`, and is
// boundary-validated against schemas/cloud_inspect_report.schema.json.
//
// It carries **no credentials**: the inspector runs the read-only commands but
// never reads or stores the provider's config/credential files.
struct CloudInspectReport {
    std::string provider_id;
    std::string provider_name;
    bool ran = false;            // an inspect actually executed (runnable provider)
    bool reachable = false;      // at least one command exited 0
    bool authenticated = false;  // the identity command succeeded with an identity
    std::string identity;        // account / arn / user / project / namespace
    std::vector<std::string> locations;  // regions / locations
    std::vector<std::string> resources;  // k8s nodes, etc.
    std::vector<CloudInspectCommand> commands;
    std::string detail;          // an overall note (CLI missing, parse error, …)
    std::string checked_at;      // ISO-8601 stamp of when the inspect ran
};

// Runs a provider's read-only `inspect_commands` and projects their output into a
// CloudInspectReport. **It is the real cloud read** — but it performs NO consent
// itself: callers (the GUI consent gate) must obtain explicit, per-action,
// deny-by-default permission BEFORE calling inspect() (CLAUDE.md hard rule).
//
// It runs every command through the shared CommandRunner seam (argv, no shell),
// so it is testable without any cloud CLI installed (inject a fake runner) and
// free of shell-injection risk. Each command's JSON is parsed non-throwing and
// validated against an embedded schema before any field is read; a missing CLI,
// a non-zero exit, or malformed JSON degrades gracefully (the report records the
// failure, never throws). A provider that isn't `runnable()` (bare-metal) yields
// a report with `ran = false`.
class CloudInspector {
public:
    // An empty runner means "use the default exec-based runner"
    // (default_command_runner) — the production path. Tests inject a fake.
    explicit CloudInspector(CommandRunner runner = {});

    CloudInspectReport inspect(const CloudProvider& provider) const;

private:
    CommandRunner runner_;
};

// JSON form + boundary validation for the report (zetta's own contract), mirroring
// the Run-record pattern. to_json round-trips through validate_cloud_inspect_report.
nlohmann::json to_json(const CloudInspectReport& report);
const nlohmann::json& cloud_inspect_report_schema();
bool validate_cloud_inspect_report(const nlohmann::json& value,
                                   std::string* error = nullptr);
std::optional<CloudInspectReport> parse_cloud_inspect_report(
    const nlohmann::json& value, std::string* error = nullptr);

}  // namespace zetta
