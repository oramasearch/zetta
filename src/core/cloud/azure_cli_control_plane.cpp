#include "zetta/cloud/azure_cli_control_plane.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/command_runner.hpp"
#include "zetta/cloud/secret_file.hpp"

namespace zetta::cloud {

namespace {

// Append a flag and its value to an argv vector (kept terse — the binding builds
// several of these). No shell is involved: argv is exec'd directly.
void flag(std::vector<std::string>& argv, std::string name, std::string value) {
    argv.push_back(std::move(name));
    argv.push_back(std::move(value));
}

// The flags every `az vmss` call shares: which subscription / resource group /
// scale set the burst pool is. The resource group is passed explicitly (not taken
// from config) because a burst picks a fresh, never-reused group name — see
// pick_resource_group.
void append_pool(std::vector<std::string>& argv, const AzureConfig& config,
                 const std::string& resource_group) {
    flag(argv, "--subscription", config.subscription_id);
    flag(argv, "--resource-group", resource_group);
    flag(argv, "--name", config.pool_name);
}

// Choose a resource group to burst into that is neither in use nor mid-deletion:
// try `<base>-01`, `-02`, … and take the first that does not already exist. A
// group that is deprovisioning still "exists" (az group create on it fails with
// ResourceGroupBeingDeleted), so a delete + immediate re-add would otherwise race
// on the same fixed name — picking the next free number sidesteps that entirely.
std::string pick_resource_group(const CommandRunner& runner,
                                const AzureConfig& config) {
    const std::string base =
        config.resource_group.empty() ? "zetta-node-group" : config.resource_group;
    for (int n = 1; n <= 99; ++n) {
        const std::string suffix = (n < 10 ? "-0" : "-") + std::to_string(n);
        const std::string name = base + suffix;
        const std::vector<std::string> exists{
            "az",   "group",          "exists", "--subscription",
            config.subscription_id, "--name", name};
        const CommandResult result = runner(exists);
        // `az group exists` prints "true" (in use — including a deprovisioning
        // group) or "false". Take the first name that isn't clearly "true"; if the
        // check itself errors, use this name too (group create will surface any
        // real problem) rather than spinning through all 99.
        if (result.exit_code != 0 ||
            result.stdout_text.find("true") == std::string::npos) {
            return name;
        }
    }
    return base + "-01";  // fallback (practically unreachable)
}

// Parse an `az -o json` array of ProvisionedNode-shaped objects, skipping
// anything malformed at the validated boundary. A production binding projects
// `az vmss list-instances` output into this shape (instance → { node_id, address,
// capability }); the reference binding consumes that shape directly so its
// parsing stays aligned with ProvisionedNode::from_json.
std::vector<ProvisionedNode> parse_nodes(const std::string& json_text) {
    std::vector<ProvisionedNode> nodes;
    const nlohmann::json document =
        nlohmann::json::parse(json_text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_array()) return nodes;
    for (const nlohmann::json& element : document) {
        if (std::optional<ProvisionedNode> node = ProvisionedNode::from_json(element)) {
            nodes.push_back(std::move(*node));
        }
    }
    return nodes;
}

// The cloud-init custom-data each VM boots with: the cluster seed address and,
// on a secure cluster, the join token (#670), as KEY=VALUE lines the bootstrap
// reads. Empty when there is nothing to hand over.
std::string custom_data_blob(const AzureConfig& config) {
    std::string custom_data;
    if (!config.seed_peer.empty()) {
        custom_data = "ZETTA_SEED_PEER=" + config.seed_peer;
    }
    if (!config.join_token.empty()) {
        if (!custom_data.empty()) custom_data += '\n';
        custom_data += "ZETTA_CLUSTER_TOKEN=" + config.join_token;
    }
    return custom_data;
}

// Build the `az vmss create` command for one candidate VM SKU. The Flexible shape
// is Microsoft-Learn-verified (docs/research/azure-cli-compute.md §2.7): explicit
// fault-domain-count + single-placement-group=false, an image + admin login, SSH
// keys generated locally, and the cluster seed handed to each node's cloud-init.
// `custom_data` is the `--custom-data` argument: the blob itself, or `@<file>`
// when it carries the join token (#770 — a secret is never an argv value).
std::vector<std::string> build_vmss_create(const AzureConfig& config,
                                           const ProvisionRequest& request,
                                           const std::string& vm_sku,
                                           const std::string& resource_group,
                                           const std::string& custom_data) {
    std::vector<std::string> create{"az", "vmss", "create"};
    append_pool(create, config, resource_group);
    flag(create, "--location", config.location);
    flag(create, "--orchestration-mode", "Flexible");
    flag(create, "--platform-fault-domain-count", "1");
    flag(create, "--single-placement-group", "false");
    flag(create, "--instance-count", std::to_string(request.count));
    flag(create, "--image", config.image.empty() ? "Ubuntu2204" : config.image);
    flag(create, "--vm-sku", vm_sku);
    flag(create, "--admin-username", "azureuser");
    create.push_back("--generate-ssh-keys");
    if (config.spot) {
        flag(create, "--priority", "Spot");
        flag(create, "--eviction-policy", "Delete");
        flag(create, "--max-price", std::to_string(config.max_price));
    }
    if (!config.managed_identity.empty()) {
        flag(create, "--assign-identity", config.managed_identity);
    }
    // Hand each VM the cluster seed address via cloud-init custom-data — and, on a
    // secure cluster, the join token (#670) — so it boots with the equivalent of
    // `zetta --peer <seed> --join-token <token>`. Custom-data is readable from the
    // VM's metadata endpoint and persists on its disk (/var/lib/waagent): see
    // docs/cluster/security.md for the trade-off and the Key Vault follow-up.
    if (!custom_data.empty()) {
        flag(create, "--custom-data", custom_data);
    }
    return create;
}

// The ordered VM SKUs to try, most-preferred first: the operator's pinned SKU (if
// any), then a fallback list of small general-purpose SKUs across families and
// generations. Azure exposes NO real-time capacity API — `az vm list-skus` shows
// only subscription/policy restrictions, not live capacity — so a burst that hits
// `SkuNotAvailable (Capacity Restrictions)` on one SKU must simply try the next.
// Retry-with-fallback is the only reliable placement strategy (Microsoft Learn:
// error-sku-not-available; the same pattern AKS's list-vm-skus prescribes).
std::vector<std::string> sku_candidates(const AzureConfig& config) {
    static const char* const kFallback[] = {
        "Standard_B2s", "Standard_D2s_v5", "Standard_D2as_v5",
        "Standard_DS1_v2", "Standard_D2s_v3",
    };
    std::vector<std::string> candidates;
    if (!config.vm_size.empty()) candidates.push_back(config.vm_size);
    for (const char* sku : kFallback) {
        if (std::find(candidates.begin(), candidates.end(), sku) ==
            candidates.end()) {
            candidates.emplace_back(sku);
        }
    }
    return candidates;
}

// True when a create failure is a live-capacity shortage — the only kind worth
// retrying on a different SKU. A config / auth / quota error fails identically on
// the next SKU, so it is reported as-is rather than churning through the list.
bool is_capacity_failure(const std::string& error_text) {
    return error_text.find("SkuNotAvailable") != std::string::npos ||
           error_text.find("Capacity") != std::string::npos ||
           error_text.find("Allocation") != std::string::npos;
}

// Delete the burst scale set (best-effort, blocking) between fallback attempts —
// a create that failed on capacity leaves a partial scale set behind, and reusing
// the stable pool name for the next SKU otherwise 409-conflicts with it.
void delete_pool(const CommandRunner& runner, const AzureConfig& config,
                 const std::string& resource_group) {
    std::vector<std::string> del{"az", "vmss", "delete"};
    append_pool(del, config, resource_group);
    runner(del);
}

}  // namespace

AzCliControlPlane::AzCliControlPlane(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

ProvisionResult AzCliControlPlane::bring_up(const AzureConfig& config,
                                            const ProvisionRequest& request) {
    ProvisionResult result;

    // Pick a fresh resource group name (zetta-node-group-NN) that isn't in use or
    // mid-deletion, so a delete + immediate re-add never races on a deprovisioning
    // group (ResourceGroupBeingDeleted). The whole burst — group, scale set, and any
    // SKU-fallback cleanup — lives in this one picked group.
    const std::string resource_group = pick_resource_group(runner_, config);

    // Azure requires the resource group to exist before any resource in it. Create
    // it up front (a resource group is free); the picked name guarantees it's fresh.
    std::vector<std::string> ensure_group{"az", "group", "create"};
    flag(ensure_group, "--subscription", config.subscription_id);
    flag(ensure_group, "--name", resource_group);
    flag(ensure_group, "--location", config.location);
    flag(ensure_group, "--output", "none");
    if (const CommandResult grouped = runner_(ensure_group);
        grouped.exit_code != 0) {
        result.satisfied = false;
        result.detail = "az group create failed: " +
                        (grouped.stderr_text.empty() ? grouped.stdout_text
                                                     : grouped.stderr_text);
        return result;
    }

    // Create the burst pool, falling back across candidate SKUs on a live-capacity
    // shortage. Azure has no capacity API, so the only reliable way to place a VM
    // is to try a SKU and, if it returns `SkuNotAvailable (Capacity Restrictions)`,
    // clean up the partial scale set and try the next.
    //
    // The custom-data carries the join token on a secure cluster, so it goes to
    // `az` as `@<file>` (a private file, removed after the burst) rather than as
    // an argument any local user could read from the process list (#770).
    std::string custom_data = custom_data_blob(config);
    std::unique_ptr<SecretFile> custom_data_file;
    if (!config.join_token.empty()) {
        std::string reason;
        custom_data_file = SecretFile::create(custom_data, &reason);
        if (!custom_data_file) {
            result.satisfied = false;
            result.detail = "cannot stage the cloud-init custom-data: " + reason;
            return result;
        }
        custom_data = "@" + custom_data_file->path().string();
    }
    const std::vector<std::string> candidates = sku_candidates(config);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const std::string& vm_sku = candidates[i];
        const CommandResult created = runner_(
            build_vmss_create(config, request, vm_sku, resource_group, custom_data));

        if (created.exit_code == 0) {
            // The scale set placed the requested VMs. They boot, read the seed peer
            // from cloud-init, and join the cluster by gossip — so they surface in
            // Cluster::nodes() as they come up, not necessarily via list-instances
            // yet (the instance→cluster-node projection is a later slice). Report the
            // add as satisfied on a successful placement rather than on list().
            result.satisfied = true;
            result.detail = std::to_string(request.count) + " node(s) provisioning on " +
                            vm_sku + " in group '" + resource_group +
                            "' — they join the cluster as they boot";
            return result;
        }

        const std::string error = created.stderr_text.empty()
                                      ? created.stdout_text
                                      : created.stderr_text;
        result.satisfied = false;
        result.detail = "az vmss create failed for " + vm_sku + ": " + error;

        // A non-capacity error (config / auth / quota) fails identically on every
        // SKU, and the last candidate has nowhere to fall back to — report either.
        if (!is_capacity_failure(error) || i + 1 == candidates.size()) {
            return result;
        }
        // Capacity shortage with candidates left: free the pool name (the failed
        // scale set would 409-conflict the retry), then try the next SKU.
        delete_pool(runner_, config, resource_group);
    }

    return result;  // unreachable — the loop always returns
}

bool AzCliControlPlane::destroy(const AzureConfig& config,
                                const std::string& node_id) {
    std::vector<std::string> remove{"az", "vmss", "delete-instances"};
    append_pool(remove, config, config.resource_group);
    flag(remove, "--instance-ids", node_id);
    return runner_(remove).exit_code == 0;
}

std::vector<ProvisionedNode> AzCliControlPlane::list(
    const AzureConfig& config) const {
    std::vector<std::string> query{"az", "vmss", "list-instances"};
    append_pool(query, config, config.resource_group);
    flag(query, "--output", "json");

    const CommandResult listed = runner_(query);
    if (listed.exit_code != 0) return {};
    return parse_nodes(listed.stdout_text);
}

}  // namespace zetta::cloud