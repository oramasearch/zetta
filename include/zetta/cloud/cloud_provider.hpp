#pragma once

#include <string>
#include <vector>

namespace zetta {

// One read-only inspect command a provider would run: a human `display` string
// (shown verbatim in the consent gate) plus the `argv` that actually runs it —
// program + arguments, **no shell** (so it goes straight to the shared
// CommandRunner, free of shell-injection risk; see command_runner.hpp).
//
// `argv` is EMPTY when the command can't run as-is here — e.g. bare-metal's
// `ssh <host> …`, which needs a concrete host before it is runnable. A provider
// is `runnable()` only when at least one of its inspect commands has a non-empty
// argv; the consent gate still *names* a display-only command so the user sees
// the full shape of what an inspect entails.
struct InspectCommand {
    std::string display;            // "aws sts get-caller-identity --output json"
    std::vector<std::string> argv;  // {"aws","sts","get-caller-identity",…}; empty = not runnable
};

// A cloud / compute provider zetta can target — the descriptive, read-only
// metadata the **Cloud Inspector** renders (FEAT-4 family; the GUI surface over
// the existing provisioner + CLI-control-plane backends).
//
// **This is PURE METADATA. It performs no cloud access of any kind** — no SDK,
// no CLI, no file read, no network call. It only *describes* what inspecting a
// provider would touch, so the UI can name it in the consent gate *before*
// anything happens. The CloudInspector (cloud_inspector.hpp) is what actually
// runs a provider's `inspect_commands`, and only behind explicit consent.
//
// HARD RULE (see CLAUDE.md → "Cloud providers — permission-gated"): reading any
// of `config_paths`, or running any of `inspect_commands`, or any other read /
// write to a provider, is gated behind explicit, per-action, deny-by-default
// user consent. The CloudInspector reads only what the read-only
// `inspect_commands` return (an identity/inventory) — it never reads or prints
// the `config_paths` file contents (those would expose secrets); they are named
// in the gate only so the user sees what the CLI may touch.
struct CloudProvider {
    std::string id;        // stable slug ("aws", "gcp", "kubernetes", …); unique
    std::string name;      // display name ("Amazon Web Services")
    std::string category;  // grouping ("Managed cloud" | "Kubernetes" |
                           // "On-prem" | "Local")
    std::string summary;   // one-line description of what it targets
    std::string cli;       // the CLI an inspect would use ("aws", "gcloud", "az",
                           // "kubectl", "oci", "ssh"); empty = none (Local)

    // What an inspect WOULD read — surfaced verbatim in the consent gate so the
    // user sees exactly what they are authorizing. Read-only; never written, and
    // never read/printed by the inspector itself. Empty for Local.
    std::vector<std::string> config_paths;     // e.g. "~/.aws/credentials"
    // What an inspect WOULD run — read-only identity / inventory commands only;
    // no provisioning, deploy, or teardown. Empty for Local.
    std::vector<InspectCommand> inspect_commands;

    // True when inspecting this provider would touch anything outside this node
    // (a config file or a command) — i.e. when consent is required before an
    // inspect may run. Local reads nothing, so it returns false; every cloud /
    // Kubernetes / on-prem provider returns true.
    bool requires_consent() const {
        return !config_paths.empty() || !inspect_commands.empty();
    }

    // True when at least one inspect command can actually run here (a non-empty
    // argv). bare-metal is described but not yet runnable (its `ssh <host>` needs
    // a concrete host), so it requires consent but is not runnable.
    bool runnable() const {
        for (const InspectCommand& command : inspect_commands) {
            if (!command.argv.empty()) return true;
        }
        return false;
    }
};

// An ordered, read-only catalog of the providers zetta can target. Mirrors the
// SolutionCatalog shape (a built-in singleton + find-by-id), but holds only
// metadata — there is nothing to parse or validate at a boundary because no
// untrusted input is involved.
class CloudProviderCatalog {
public:
    explicit CloudProviderCatalog(std::vector<CloudProvider> providers)
        : providers_(std::move(providers)) {}

    // The built-in catalog: a process-wide, immutable list of the provider
    // backends (local, bare-metal, Kubernetes, AWS, GCP, Azure, OCI), ordered
    // local → on-prem → Kubernetes → managed clouds.
    static const CloudProviderCatalog& builtin();

    const std::vector<CloudProvider>& providers() const { return providers_; }

    // The provider with this id, or nullptr if there is none. A **non-owning**
    // observer into the catalog's stable storage, valid for the catalog's
    // lifetime (builtin() is a static singleton).
    const CloudProvider* find(const std::string& id) const;

    std::size_t size() const { return providers_.size(); }
    bool empty() const { return providers_.empty(); }

private:
    std::vector<CloudProvider> providers_;
};

}  // namespace zetta
