#pragma once

#include <optional>
#include <string>
#include <vector>

#include "zetta/aws_cli_control_plane.hpp"
#include "zetta/aws_provisioner.hpp"
#include "zetta/azure_cli_control_plane.hpp"
#include "zetta/azure_provisioner.hpp"
#include "zetta/bare_metal_cli_control_plane.hpp"
#include "zetta/bare_metal_provisioner.hpp"
#include "zetta/gcp_cli_control_plane.hpp"
#include "zetta/gcp_provisioner.hpp"
#include "zetta/k8s_cli_control_plane.hpp"
#include "zetta/k8s_provisioner.hpp"
#include "zetta/oci_cli_control_plane.hpp"
#include "zetta/oci_provisioner.hpp"
#include "zetta/provisioner_config.hpp"

namespace zetta::gui {

class App;

// CloudProvisioners — wires the cloud-burst Provisioner backends (Azure / AWS /
// GCP / OCI / Kubernetes / bare-metal) into the headed App's "Add nodes" control
// from the node's detected provisioner configs, and **owns** them for the App's
// lifetime.
//
// The "Add nodes" consent gate is deny-by-default: a cloud target's "Allow"
// button only appears once a configured Provisioner has been injected for that
// target (App::set_provisioner). The configuration is not a file and carries no
// credentials: it is the deployment context detected from the operator's own
// local CLI (`detect_provisioner_configs`, surfaced as system.zetta_provisioners),
// and the reference CLI control plane uses that same CLI's credentials to act. A
// provider with no detected config simply stays unwired (the gate then reads "no
// provisioner wired", unchanged) — nothing is provisioned that the local CLI did
// not already have a context for. Every cloud add stays per-action consent-gated
// in the GUI and is never remembered.
//
// Each provisioner holds a reference to its control plane, and both must outlive
// App::run(); this holder keeps them in std::optional members (conditional
// construction with a stable address — emplaced once, never moved), so a
// CloudProvisioners instance must be declared before, and thus outlive, the App
// it wires.
class CloudProvisioners {
public:
    // One provider's wiring outcome, for the startup log.
    struct Wiring {
        std::string id;      // catalog id ("azure", "aws", …)
        bool wired = false;  // a valid config was found and the provisioner injected
        std::string detail;  // why it was skipped, when not wired
    };

    // For every detected provisioner config in `configs` whose target is a known
    // cloud provider, inject `default_seed_peer` into any config that did not set
    // its own seed (so burst nodes auto-join this cluster), construct the reference
    // CLI control plane + provisioner, and register it with `app`. Returns a
    // per-provider outcome for every known cloud provider; a provider with no
    // detected config is reported skipped. Never throws — a malformed detected
    // config is simply skipped, matching the seam's no-throw contract.
    std::vector<Wiring> wire(App& app,
                             const std::vector<ProvisionerConfig>& configs,
                             const std::string& default_seed_peer);

private:
    std::optional<AzCliControlPlane> azure_cp_;
    std::optional<AzureProvisioner> azure_;
    std::optional<AwsCliControlPlane> aws_cp_;
    std::optional<AwsProvisioner> aws_;
    std::optional<GcpCliControlPlane> gcp_cp_;
    std::optional<GcpProvisioner> gcp_;
    std::optional<OciCliControlPlane> oci_cp_;
    std::optional<OciProvisioner> oci_;
    std::optional<K8sCliControlPlane> k8s_cp_;
    std::optional<K8sProvisioner> k8s_;
    std::optional<BareMetalCliControlPlane> bare_metal_cp_;
    std::optional<BareMetalProvisioner> bare_metal_;
};

}  // namespace zetta::gui
