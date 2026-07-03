#include "zetta/cloud/oci_cli_control_plane.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/command_runner.hpp"

namespace zetta::cloud {

namespace {

// Append a flag and its value to an argv vector (kept terse — the binding builds
// several of these). No shell is involved: argv is exec'd directly.
void flag(std::vector<std::string>& argv, std::string name, std::string value) {
    argv.push_back(std::move(name));
    argv.push_back(std::move(value));
}

// Add `--region <r>` when the config pins a region (otherwise the CLI uses the
// profile's region). Shared by every `oci` call.
void append_region(std::vector<std::string>& argv, const OciConfig& config) {
    if (!config.region.empty()) flag(argv, "--region", config.region);
}

// Trim ASCII whitespace from both ends — the `--raw-output` stdout an `oci`
// `--query` produces is a bare value plus a trailing newline.
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// An OCID stdout is "absent" when empty or the literal "null" a JMESPath miss
// prints — both mean "no such resource".
bool is_absent(const std::string& ocid) { return ocid.empty() || ocid == "null"; }

// Parse an `oci` JSON listing of ProvisionedNode-shaped objects, skipping anything
// malformed at the validated boundary. The `oci` CLI wraps results in a top-level
// { "data": [ ... ] } envelope, so accept that as well as a bare array. A
// production binding projects `instance-pool list-instances` output into the
// { node_id, address, capability } shape (via `--query`); the reference binding
// consumes that shape directly so its parsing stays aligned with
// ProvisionedNode::from_json.
std::vector<ProvisionedNode> parse_nodes(const std::string& json_text) {
    std::vector<ProvisionedNode> nodes;
    nlohmann::json document =
        nlohmann::json::parse(json_text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) return nodes;
    if (document.is_object() && document.contains("data")) document = document["data"];
    if (!document.is_array()) return nodes;
    for (const nlohmann::json& element : document) {
        if (std::optional<ProvisionedNode> node = ProvisionedNode::from_json(element)) {
            nodes.push_back(std::move(*node));
        }
    }
    return nodes;
}

// Build the instance-configuration `--instance-details` JSON: the per-node launch
// shape (compartment / AD / shape + shapeConfig / image / VNIC / cloud-init seed /
// preemptible). On OCI the per-node shape lives in the instance configuration the
// pool instantiates — preemptible is a launch property (preemptibleInstanceConfig),
// which is why spot/seed land here, not on the pool create.
std::string instance_details_json(const OciConfig& config) {
    nlohmann::json launch{
        {"compartmentId", config.compartment_id},
        {"availabilityDomain", config.availability_domain},
        {"displayName", config.pool_name + "-member"},
        {"shape", config.shape},
        {"createVnicDetails", {{"subnetId", config.subnet_id}, {"assignPublicIp", false}}},
    };
    if (config.ocpus > 0) {
        launch["shapeConfig"] = {{"ocpus", config.ocpus},
                                 {"memoryInGBs", config.memory_in_gbs}};
    }
    if (!config.image_id.empty()) {
        launch["sourceDetails"] = {{"sourceType", "image"}, {"imageId", config.image_id}};
    }
    if (!config.seed_peer.empty()) {
        // Hand each node the cluster seed address at boot via cloud-init user_data.
        // (A real deployment supplies a full base64 cloud-init; this illustrative
        // value carries the seed peer the bootstrap reads.)
        launch["metadata"] = {{"user_data", "ZETTA_SEED_PEER=" + config.seed_peer}};
    }
    if (config.spot) {
        launch["preemptibleInstanceConfig"] = {
            {"preemptionAction", {{"type", "TERMINATE"}, {"preserveBootVolume", false}}}};
    }
    return nlohmann::json{{"instanceType", "compute"}, {"launchDetails", launch}}.dump();
}

// Build the instance-pool `--placement-configurations` JSON.
std::string placement_json(const OciConfig& config) {
    return nlohmann::json::array(
               {{{"availabilityDomain", config.availability_domain},
                 {"primarySubnetId", config.subnet_id}}})
        .dump();
}

}  // namespace

OciCliControlPlane::OciCliControlPlane(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

std::string OciCliControlPlane::resolve_pool_id(const OciConfig& config) const {
    std::vector<std::string> query{"oci", "compute-management", "instance-pool",
                                   "list"};
    flag(query, "--compartment-id", config.compartment_id);
    flag(query, "--display-name", config.pool_name);
    flag(query, "--query", "data[0].id");
    query.push_back("--raw-output");
    append_region(query, config);

    const CommandResult listed = runner_(query);
    if (listed.exit_code != 0) return "";
    const std::string ocid = trim(listed.stdout_text);
    return is_absent(ocid) ? "" : ocid;
}

ProvisionResult OciCliControlPlane::bring_up(const OciConfig& config,
                                             const ProvisionRequest& request) {
    ProvisionResult result;

    // OCI addresses resources by OCID, not name — resolve the burst pool's OCID
    // from its stable display name to keep a re-burst idempotent (resize an
    // existing pool rather than create a duplicate).
    std::string pool_id = resolve_pool_id(config);

    if (pool_id.empty()) {
        // Create path: ensure an instance configuration (the per-node template),
        // then create the Instance Pool from it at the requested size.
        std::vector<std::string> ensure_config{
            "oci", "compute-management", "instance-configuration", "create"};
        flag(ensure_config, "--compartment-id", config.compartment_id);
        flag(ensure_config, "--display-name", config.pool_name + "-config");
        flag(ensure_config, "--instance-details", instance_details_json(config));
        flag(ensure_config, "--query", "data.id");
        ensure_config.push_back("--raw-output");
        append_region(ensure_config, config);
        const std::string config_id = trim(runner_(ensure_config).stdout_text);

        std::vector<std::string> create{"oci", "compute-management",
                                        "instance-pool", "create"};
        flag(create, "--compartment-id", config.compartment_id);
        flag(create, "--instance-configuration-id", config_id);
        flag(create, "--placement-configurations", placement_json(config));
        flag(create, "--size", std::to_string(request.count));
        flag(create, "--display-name", config.pool_name);
        flag(create, "--query", "data.id");
        create.push_back("--raw-output");
        append_region(create, config);
        const CommandResult created = runner_(create);
        if (created.exit_code != 0) {
            result.satisfied = false;
            result.detail = "oci instance-pool create failed: " +
                            (created.stderr_text.empty() ? created.stdout_text
                                                         : created.stderr_text);
            return result;
        }
        pool_id = trim(created.stdout_text);
    } else {
        // Resize path: scale the existing pool to the requested size.
        std::vector<std::string> resize{"oci", "compute-management",
                                        "instance-pool", "update"};
        flag(resize, "--instance-pool-id", pool_id);
        flag(resize, "--size", std::to_string(request.count));
        append_region(resize, config);
        const CommandResult resized = runner_(resize);
        if (resized.exit_code != 0) {
            result.satisfied = false;
            result.detail = "oci instance-pool update failed: " +
                            (resized.stderr_text.empty() ? resized.stdout_text
                                                         : resized.stderr_text);
            return result;
        }
    }

    // The pool is scaling; list what is up (using the pool id we just
    // created/resolved — no need to re-resolve) and report best-effort against the
    // requested count.
    result.nodes = list_instances(config, pool_id);
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

bool OciCliControlPlane::destroy(const OciConfig& config,
                                 const std::string& node_id) {
    // Detaching needs the pool OCID; resolve it. No pool ⇒ nothing to detach.
    const std::string pool_id = resolve_pool_id(config);
    if (pool_id.empty()) return false;

    std::vector<std::string> detach{"oci", "compute-management",
                                    "instance-pool-instance", "detach"};
    flag(detach, "--instance-pool-id", pool_id);
    flag(detach, "--instance-id", node_id);
    // Terminate the instance (delete it + its boot volume) and shrink the pool, so
    // it is not replaced — the per-node teardown the seam contract wants.
    flag(detach, "--is-auto-terminate", "true");
    flag(detach, "--is-decrement-size", "true");
    append_region(detach, config);
    return runner_(detach).exit_code == 0;
}

std::vector<ProvisionedNode> OciCliControlPlane::list(
    const OciConfig& config) const {
    return list_instances(config, resolve_pool_id(config));
}

std::vector<ProvisionedNode> OciCliControlPlane::list_instances(
    const OciConfig& config, const std::string& pool_id) const {
    if (pool_id.empty()) return {};

    std::vector<std::string> query{"oci", "compute-management", "instance-pool",
                                   "list-instances"};
    flag(query, "--compartment-id", config.compartment_id);
    flag(query, "--instance-pool-id", pool_id);
    append_region(query, config);

    const CommandResult listed = runner_(query);
    if (listed.exit_code != 0) return {};
    return parse_nodes(listed.stdout_text);
}

}  // namespace zetta::cloud