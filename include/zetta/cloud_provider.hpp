#pragma once

#include <string>
#include <vector>

namespace zetta {

// A cloud / compute provider zetta can target — the descriptive, read-only
// metadata the **Cloud Inspector** renders (FEAT-4 family; the GUI surface over
// the existing provisioner + CLI-control-plane backends).
//
// **This is PURE METADATA. It performs no cloud access of any kind** — no SDK,
// no CLI, no file read, no network call. It only *describes* what inspecting a
// provider would touch, so the UI can name it in the consent gate *before*
// anything happens.
//
// HARD RULE (see CLAUDE.md → "Cloud providers — permission-gated"): reading any
// of `config_paths`, or running any of `inspect_commands`, or any other read /
// write to a provider, is gated behind explicit, per-action, deny-by-default
// user consent. Wiring those real reads behind the gate is a later slice; this
// type carries only the description the gate shows.
struct CloudProvider {
    std::string id;        // stable slug ("aws", "gcp", "kubernetes", …); unique
    std::string name;      // display name ("Amazon Web Services")
    std::string category;  // grouping ("Managed cloud" | "Kubernetes" |
                           // "On-prem" | "Local")
    std::string summary;   // one-line description of what it targets
    std::string cli;       // the CLI an inspect would use ("aws", "gcloud", "az",
                           // "kubectl", "oci", "ssh"); empty = none (Local)

    // What an inspect WOULD read — surfaced verbatim in the consent gate so the
    // user sees exactly what they are authorizing. Read-only; never written.
    // Empty for Local (it reads nothing — it is this running node).
    std::vector<std::string> config_paths;     // e.g. "~/.aws/credentials"
    // What an inspect WOULD run — read-only identity / inventory commands only;
    // no provisioning, deploy, or teardown. Empty for Local.
    std::vector<std::string> inspect_commands;  // e.g. "aws sts get-caller-identity"

    // True when inspecting this provider would touch anything outside this node
    // (a config file or a command) — i.e. when consent is required before an
    // inspect may run. Local reads nothing, so it returns false; every cloud /
    // Kubernetes / on-prem provider returns true.
    bool requires_consent() const {
        return !config_paths.empty() || !inspect_commands.empty();
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
