#include "zetta/azure_cli_control_plane.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/command_runner.hpp"

namespace zetta {

namespace {

// Append a flag and its value to an argv vector (kept terse — the binding builds
// several of these). No shell is involved: argv is exec'd directly.
void flag(std::vector<std::string>& argv, std::string name, std::string value) {
    argv.push_back(std::move(name));
    argv.push_back(std::move(value));
}

// The flags every `az vmss` call shares: which subscription / resource group /
// scale set the burst pool is.
void append_pool(std::vector<std::string>& argv, const AzureConfig& config) {
    flag(argv, "--subscription", config.subscription_id);
    flag(argv, "--resource-group", config.resource_group);
    flag(argv, "--name", config.pool_name);
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

// Build the `az vmss create` command for one candidate VM SKU. The Flexible shape
// is Microsoft-Learn-verified (docs/research/azure-cli-compute.md §2.7): explicit
// fault-domain-count + single-placement-group=false, an image + admin login, SSH
// keys generated locally, and the cluster seed handed to each node's cloud-init.
std::vector<std::string> build_vmss_create(const AzureConfig& config,
                                           const ProvisionRequest& request,
                                           const std::string& vm_sku) {
    std::vector<std::string> create{"az", "vmss", "create"};
    append_pool(create, config);
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
    if (!config.seed_peer.empty()) {
        flag(create, "--custom-data", "ZETTA_SEED_PEER=" + config.seed_peer);
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
void delete_pool(const CommandRunner& runner, const AzureConfig& config) {
    std::vector<std::string> del{"az", "vmss", "delete"};
    append_pool(del, config);
    runner(del);
}

}  // namespace

AzCliControlPlane::AzCliControlPlane(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

ProvisionResult AzCliControlPlane::bring_up(const AzureConfig& config,
                                            const ProvisionRequest& request) {
    ProvisionResult result;

    // Azure requires the resource group to exist before any resource in it, so a
    // first burst into a fresh group otherwise fails with ResourceGroupNotFound.
    // Ensure it up front — `az group create` is idempotent (re-creating an existing
    // group is a no-op success) and a resource group is free — so "add nodes" just
    // works against a subscription with no pre-provisioned group. Tearing the whole
    // group down is the clean uninstall (a later slice).
    std::vector<std::string> ensure_group{"az", "group", "create"};
    flag(ensure_group, "--subscription", config.subscription_id);
    flag(ensure_group, "--name", config.resource_group);
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
    // clean up the partial scale set and try the next. The stable pool name keeps a
    // second burst idempotent (it scales the existing set rather than duplicating).
    const std::vector<std::string> candidates = sku_candidates(config);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const std::string& vm_sku = candidates[i];
        const CommandResult created =
            runner_(build_vmss_create(config, request, vm_sku));

        if (created.exit_code == 0) {
            // The scale set placed the requested VMs. They boot, read the seed peer
            // from cloud-init, and join the cluster by gossip — so they surface in
            // Cluster::nodes() as they come up, not necessarily in list() yet (the
            // instance→cluster-node projection is a later slice). Report the add as
            // satisfied on a successful placement rather than on list() parsing.
            result.nodes = list(config);
            result.satisfied = true;
            result.detail = std::to_string(request.count) + " node(s) provisioning on " +
                            vm_sku + " in pool '" + config.pool_name +
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
        // Capacity shortage with candidates left: free the stable pool name (the
        // failed scale set would 409-conflict the retry), then try the next SKU.
        delete_pool(runner_, config);
    }

    return result;  // unreachable — the loop always returns
}

bool AzCliControlPlane::destroy(const AzureConfig& config,
                                const std::string& node_id) {
    std::vector<std::string> remove{"az", "vmss", "delete-instances"};
    append_pool(remove, config);
    flag(remove, "--instance-ids", node_id);
    return runner_(remove).exit_code == 0;
}

std::vector<ProvisionedNode> AzCliControlPlane::list(
    const AzureConfig& config) const {
    std::vector<std::string> query{"az", "vmss", "list-instances"};
    append_pool(query, config);
    flag(query, "--output", "json");

    const CommandResult listed = runner_(query);
    if (listed.exit_code != 0) return {};
    return parse_nodes(listed.stdout_text);
}

}  // namespace zetta
