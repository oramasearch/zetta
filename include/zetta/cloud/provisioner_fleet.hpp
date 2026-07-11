#pragma once

#include <optional>
#include <string>
#include <vector>

#include "zetta/cloud/aws_cli_control_plane.hpp"
#include "zetta/cloud/aws_provisioner.hpp"
#include "zetta/cloud/azure_cli_control_plane.hpp"
#include "zetta/cloud/azure_provisioner.hpp"
#include "zetta/cloud/bare_metal_cli_control_plane.hpp"
#include "zetta/cloud/bare_metal_provisioner.hpp"
#include "zetta/cloud/gcp_cli_control_plane.hpp"
#include "zetta/cloud/gcp_provisioner.hpp"
#include "zetta/cloud/k8s_cli_control_plane.hpp"
#include "zetta/cloud/k8s_provisioner.hpp"
#include "zetta/cloud/oci_cli_control_plane.hpp"
#include "zetta/cloud/oci_provisioner.hpp"
#include "zetta/cloud/provisioner_config.hpp"

namespace zetta::cloud {

// ProvisionerFleet — builds and **owns** the cloud-burst Provisioner backends
// (Azure / AWS / GCP / OCI / Kubernetes / bare-metal) from the node's detected
// provisioner configs, one per target the operator's local CLI already has a
// context for. Formerly the headed app's private wiring (src/gui); it lives in
// zetta-core so BOTH consumers of a wired Provisioner share one holder (#621):
// the headed "Add nodes" control (App::set_provisioner) and the MCP surface's
// consent-gated `add_nodes` capability (ApiServer::set_provisioner_resolver).
//
// The configuration is not a file and carries no credentials: it is the
// deployment context detected from the operator's own local CLI
// (`detect_provisioner_configs`, surfaced as system.provisioners), and the
// reference CLI control plane uses that same CLI's credentials to act. A
// provider with no detected config simply stays unwired — nothing can be
// provisioned that the local CLI did not already have a context for, and every
// cloud add stays per-action consent-gated (the GUI gate / the #621 challenge).
//
// Each provisioner holds a reference to its control plane, and both must
// outlive every consumer; this holder keeps them in std::optional members
// (conditional construction with a stable address — emplaced once, never
// moved), so a ProvisionerFleet must be declared before, and thus outlive, the
// App / ApiServer it supplies.
class ProvisionerFleet {
public:
    // One provider's wiring outcome, for the startup log.
    struct Wiring {
        std::string id;      // catalog id ("azure", "aws", …)
        bool wired = false;  // a valid config was found and the provisioner built
        std::string detail;  // why it was skipped, when not wired
    };

    // For every detected provisioner config in `configs` whose target is a
    // known cloud provider, inject `default_seed_peer` into any config that did
    // not set its own seed (so burst nodes auto-join this cluster) and
    // `default_cluster_token` into any config that did not set its own token (so
    // a burst node can join a SECURE, non-plaintext cluster — #670; the token is
    // this cluster's own join token, secret, injected here at wire time and never
    // stored in the detected config / system.provisioners), then construct the
    // reference CLI control plane + provisioner. Returns a per-provider outcome
    // for every known cloud provider; a provider with no detected config is
    // reported skipped. Never throws — a malformed detected config is simply
    // skipped, matching the seam's no-throw contract. `default_cluster_token` is
    // empty for a plaintext cluster, in which case no token is injected.
    std::vector<Wiring> wire(const std::vector<ProvisionerConfig>& configs,
                             const std::string& default_seed_peer,
                             const std::string& default_cluster_token = {});

    // The wired Provisioner for a catalog target id, or nullptr when that
    // target has no detected context here. Non-owning; valid for the fleet's
    // lifetime.
    Provisioner* provisioner_for(const std::string& target_id);

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

}  // namespace zetta::cloud
