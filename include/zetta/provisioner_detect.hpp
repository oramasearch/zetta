#pragma once

#include <optional>
#include <vector>

#include "zetta/command_runner.hpp"
#include "zetta/provisioner_config.hpp"

namespace zetta {

// Detect which cloud-burst provisioners this node can drive by asking the
// operator's own **local CLIs** for their active deployment context — the
// credential-free half of "add cloud nodes with the CLI you're already logged into"
// (the sibling of the consent-gated Cloud Inspector, which reads the same CLIs).
//
// Each detector runs a couple of fast, read-only, non-interactive CLI commands
// through the shared CommandRunner seam (so it is testable against a fake and never
// touches a real cloud in CI), and on success returns that provider's
// ProvisionerConfig — its non-secret targeting (subscription / region / …) as the
// provider's validated XConfig JSON. A CLI that is absent, not logged in, or
// returns nothing yields std::nullopt and the provider is simply not detected
// (left unwired in the Add-nodes control). **No credentials are read or stored** —
// the CLI keeps its own auth, and the control plane uses that same CLI to act.
//
// `detect_provisioner_configs` runs every available detector and returns what was
// found. `runner` defaults to the real exec-based runner; a test injects a fake
// that returns canned CLI output.
std::vector<ProvisionerConfig> detect_provisioner_configs(CommandRunner runner = {});

// Azure detector (the reference): `az account show` for the subscription + tenant,
// `az config get` for the operator's default resource group + location (falling
// back to a stable "zetta-burst" group + "eastus" when the CLI has no default),
// and `az account list-locations` for the deployable regions (the Region dropdown).
// std::nullopt when `az` is absent or not logged in.
std::optional<ProvisionerConfig> detect_azure_provisioner(const CommandRunner& run);

// The Azure VM sizes available (no subscription/policy restriction) for creating
// virtual machines in `location` — the options for the Add-nodes VM-size dropdown,
// via `az vm list-skus`. Region-dependent and a large/slow query, so it is fetched
// on demand (off the UI thread) rather than at detection. Empty on any CLI error.
// Live capacity is NOT reflected (Azure exposes no capacity API — the control
// plane's SKU fallback handles that at create time). `runner` defaults to the real
// exec-based runner.
std::vector<std::string> azure_vm_sizes(const std::string& subscription,
                                        const std::string& location,
                                        CommandRunner runner = {});

}  // namespace zetta
