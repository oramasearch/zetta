#include "zetta/azure_cli_control_plane.hpp"

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

    // Scale the burst pool to the requested count — create it if absent. The
    // stable pool name makes this idempotent (a second burst scales the existing
    // scale set rather than spawning a duplicate).
    std::vector<std::string> create{"az", "vmss", "create"};
    append_pool(create, config);
    flag(create, "--location", config.location);
    // Flexible orchestration is the recommended mode for self-managed scaled
    // workers (independent VMs under one scale set) — the burst-cluster shape.
    flag(create, "--orchestration-mode", "Flexible");
    flag(create, "--instance-count", std::to_string(request.count));
    if (!config.image.empty()) flag(create, "--image", config.image);
    if (!config.vm_size.empty()) flag(create, "--vm-sku", config.vm_size);
    if (config.spot) {
        flag(create, "--priority", "Spot");
        flag(create, "--eviction-policy", "Delete");
        flag(create, "--max-price", std::to_string(config.max_price));
    }
    if (!config.managed_identity.empty()) {
        flag(create, "--assign-identity", config.managed_identity);
    }
    // Hand each node the cluster seed address at boot so it auto-joins. (A real
    // deployment supplies a full cloud-init; this illustrative custom-data carries
    // the seed peer the bootstrap reads.)
    if (!config.seed_peer.empty()) {
        flag(create, "--custom-data", "ZETTA_SEED_PEER=" + config.seed_peer);
    }

    const CommandResult created = runner_(create);
    if (created.exit_code != 0) {
        result.satisfied = false;
        result.detail = "az vmss create failed: " +
                        (created.stderr_text.empty() ? created.stdout_text
                                                     : created.stderr_text);
        return result;
    }

    // The pool is scaling; list what is up and report best-effort against the
    // requested count.
    result.nodes = list(config);
    result.satisfied = result.nodes.size() >= request.count;
    if (result.satisfied) {
        result.detail = "burst pool '" + config.pool_name + "' scaled to " +
                        std::to_string(request.count) + " node(s)";
    } else {
        result.detail = "best-effort: " + std::to_string(result.nodes.size()) +
                        " of " + std::to_string(request.count) +
                        " requested nodes up in pool '" + config.pool_name + "'";
    }
    return result;
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
