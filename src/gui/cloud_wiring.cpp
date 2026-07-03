#include "gui/cloud_wiring.hpp"

#include <utility>

#include <nlohmann/json.hpp>

#include "gui/app.hpp"

namespace zetta::gui {
namespace {

// Wire one provider: parse its config from `root[id]` with the provider's own
// validated XConfig::from_json, inject the cluster seed if the config didn't set
// its own, emplace the owned control plane + provisioner into the holder's slots
// (a stable address — emplaced once, never moved, so the provisioner's
// control-plane reference stays valid for the App's lifetime), and register it
// with the App. A missing or invalid config leaves both slots empty and the
// provider unwired.
template <class Config, class ControlPlane, class Provisioner>
CloudProvisioners::Wiring wire_one(const char* id, const nlohmann::json& root,
                                   const std::string& seed_peer, App& app,
                                   std::optional<ControlPlane>& cp_slot,
                                   std::optional<Provisioner>& prov_slot) {
    CloudProvisioners::Wiring out{id, false, {}};
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
    app.set_provisioner(id, *prov_slot);
    out.wired = true;
    return out;
}

}  // namespace

std::vector<CloudProvisioners::Wiring> CloudProvisioners::wire(
    App& app, const std::vector<zetta::cloud::ProvisionerConfig>& configs,
    const std::string& default_seed_peer) {
    // Index the detected configs by catalog id (latest wins, like the node's own
    // per-target store), so wire_one can look each provider up by name. A provider
    // with no detected config is absent from the object and reported skipped.
    nlohmann::json root = nlohmann::json::object();
    for (const zetta::cloud::ProvisionerConfig& detected : configs) {
        root[detected.target] = detected.config;
    }

    std::vector<Wiring> wirings;
    wirings.push_back(wire_one<zetta::cloud::AzureConfig, zetta::cloud::AzCliControlPlane, zetta::cloud::AzureProvisioner>(
        "azure", root, default_seed_peer, app, azure_cp_, azure_));
    wirings.push_back(wire_one<zetta::cloud::AwsConfig, zetta::cloud::AwsCliControlPlane, zetta::cloud::AwsProvisioner>(
        "aws", root, default_seed_peer, app, aws_cp_, aws_));
    wirings.push_back(wire_one<zetta::cloud::GcpConfig, zetta::cloud::GcpCliControlPlane, zetta::cloud::GcpProvisioner>(
        "gcp", root, default_seed_peer, app, gcp_cp_, gcp_));
    wirings.push_back(wire_one<zetta::cloud::OciConfig, zetta::cloud::OciCliControlPlane, zetta::cloud::OciProvisioner>(
        "oci", root, default_seed_peer, app, oci_cp_, oci_));
    wirings.push_back(wire_one<zetta::cloud::K8sConfig, zetta::cloud::K8sCliControlPlane, zetta::cloud::K8sProvisioner>(
        "kubernetes", root, default_seed_peer, app, k8s_cp_, k8s_));
    wirings.push_back(
        wire_one<zetta::cloud::BareMetalConfig, zetta::cloud::BareMetalCliControlPlane, zetta::cloud::BareMetalProvisioner>(
            "bare-metal", root, default_seed_peer, app, bare_metal_cp_,
            bare_metal_));
    return wirings;
}

}  // namespace zetta::gui
