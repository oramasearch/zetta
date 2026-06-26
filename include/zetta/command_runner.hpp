#pragma once

#include <functional>
#include <string>
#include <vector>

namespace zetta {

// The result of running one external command: its exit code and captured output.
// stdout_text is what a CLI binding parses (JSON, a value); stderr_text feeds the
// `detail` note when a command fails.
struct CommandResult {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
};

// Runs one command given its argv (program + arguments, **no shell**), returning
// its captured output. This is the **shared, vendor-neutral** seam every CLI-based
// cloud provisioner binding runs through — the Azure `az` binding today
// (AzCliControlPlane), the AWS `aws` and GCP `gcloud` bindings as they land
// (#377 / #378). Injecting it keeps each binding testable without the vendor CLI
// installed (a fake returns canned output) and free of shell-injection risk (argv
// is passed directly to exec, never concatenated into a shell string). An empty
// CommandRunner means "use the default exec-based runner".
using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;

// The default real command runner: `fork`/`execvp`s the program in argv[0] with
// the given arguments and captures its merged stdout+stderr. Exposed so callers
// can wrap or replace it; the CLI control-plane bindings use it when constructed
// without one.
CommandRunner default_command_runner();

}  // namespace zetta
