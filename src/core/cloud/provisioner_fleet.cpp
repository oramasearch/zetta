#include "zetta/cloud/provisioner_fleet.hpp"

#include <utility>

#include <nlohmann/json.hpp>

namespace zetta::cloud {
namespace {

// Wire one provider: parse its config from `root[id]` with the provider's own
// validated XConfig::from_json, inject the cluster seed if the config didn't
// set its own, and emplace the owned control plane + provisioner into the
// holder's slots (a stable address — emplaced once, never moved, so the
// provisioner's control-plane reference stays valid for the fleet's lifetime).
// A missing or invalid config leaves both slots empty and the provider
// unwired.
template <class Config, class ControlPlane, class Backend>
ProvisionerFleet::Wiring wire_one(const char* id, const nlohmann::json& root,
                                  const std::string& seed_peer,
                                  std::optional<ControlPlane>& cp_slot,
                                  std::optional<Backend>& prov_slot) {
    ProvisionerFleet::Wiring out{id, false, {}};
    const auto it = root.find(id);
    if (it == root.end()) {
        out.detail = "not configured";
        return out;
    }
    std::optional<Config> config = Config::from_json(*it);
    if (!config) {
        out.detail = "invalid config — missing or malformed required fields";
        return out;
    }
    if (config->seed_peer.empty()) config->seed_peer = seed_peer;
    cp_slot.emplace();  // default CommandRunner → the real provider CLI
    prov_slot.emplace(*cp_slot, std::move(*config));
    out.wired = true;
    return out;
}

}  // namespace

std::vector<ProvisionerFleet::Wiring> ProvisionerFleet::wire(
    const std::vector<ProvisionerConfig>& configs,
    const std::string& default_seed_peer) {
    // Index the detected configs by catalog id (latest wins, like the node's
    // own per-target store), so wire_one can look each provider up by name. A
    // provider with no detected config is absent from the object and reported
    // skipped.
    nlohmann::json root = nlohmann::json::object();
    for (const ProvisionerConfig& detected : configs) {
        root[detected.target] = detected.config;
    }

    std::vector<Wiring> wirings;
    wirings.push_back(wire_one<AzureConfig, AzCliControlPlane, AzureProvisioner>(
        "azure", root, default_seed_peer, azure_cp_, azure_));
    wirings.push_back(wire_one<AwsConfig, AwsCliControlPlane, AwsProvisioner>(
        "aws", root, default_seed_peer, aws_cp_, aws_));
    wirings.push_back(wire_one<GcpConfig, GcpCliControlPlane, GcpProvisioner>(
        "gcp", root, default_seed_peer, gcp_cp_, gcp_));
    wirings.push_back(wire_one<OciConfig, OciCliControlPlane, OciProvisioner>(
        "oci", root, default_seed_peer, oci_cp_, oci_));
    wirings.push_back(wire_one<K8sConfig, K8sCliControlPlane, K8sProvisioner>(
        "kubernetes", root, default_seed_peer, k8s_cp_, k8s_));
    wirings.push_back(
        wire_one<BareMetalConfig, BareMetalCliControlPlane, BareMetalProvisioner>(
            "bare-metal", root, default_seed_peer, bare_metal_cp_,
            bare_metal_));
    return wirings;
}

Provisioner* ProvisionerFleet::provisioner_for(const std::string& target_id) {
    const auto as_pointer = [](auto& slot) -> Provisioner* {
        return slot ? &*slot : nullptr;
    };
    if (target_id == "azure") return as_pointer(azure_);
    if (target_id == "aws") return as_pointer(aws_);
    if (target_id == "gcp") return as_pointer(gcp_);
    if (target_id == "oci") return as_pointer(oci_);
    if (target_id == "kubernetes") return as_pointer(k8s_);
    if (target_id == "bare-metal") return as_pointer(bare_metal_);
    return nullptr;
}

}  // namespace zetta::cloud
