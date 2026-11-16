// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::cloud {

// ProvisionerConfig — one cloud-burst provisioner's deployment context, detected
// from the operator's own local CLI (`detect_provisioner_configs`) and held on the
// Node as node-local observed state (like a CloudInspectReport — not replicated,
// not persisted), surfaced as the system.provisioners SQL table.
//
// It carries **no credentials**: the local CLI (`az` / `aws` / `gcloud` / `oci` /
// `kubectl` / `docker`) keeps its own auth, and the reference CLI control plane
// uses that same CLI to act. What lives here is only the non-secret targeting the
// CLI reported (subscription / project / region / resource group / …), held as the
// provider's own validated XConfig JSON (`config`) so the GUI wiring can rebuild a
// real provisioner from it. `summary` is a one-line, human-readable digest for the
// system table + startup log.
struct ProvisionerConfig {
    std::string target;     // catalog id: "azure" / "aws" / "gcp" / "oci" / "kubernetes" / "bare-metal"
    std::string cli;        // the local CLI it drives: "az" / "aws" / "gcloud" / …
    std::string summary;    // human one-line (e.g. "subscription <id> · <group> · <region>")
    nlohmann::json config;  // the provider's validated XConfig::to_json() shape

    // The regions the subscription can deploy to (detected from the CLI, e.g.
    // `az account list-locations`) — the options for the Add-nodes Region dropdown.
    // VM-size options are region-dependent + a large/slow query, so they are fetched
    // on demand in the GUI rather than stored here.
    std::vector<std::string> available_regions;

    bool operator==(const ProvisionerConfig&) const = default;
};

}  // namespace zetta::cloud