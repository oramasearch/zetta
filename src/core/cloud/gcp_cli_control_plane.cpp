#include "zetta/cloud/gcp_cli_control_plane.hpp"

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

// The flags every MIG `gcloud` call shares: which project / zone / MIG the burst
// pool is. (Instance templates are global, so they take only --project.)
void append_pool(std::vector<std::string>& argv, const GcpConfig& config) {
    flag(argv, "--project", config.project_id);
    flag(argv, "--zone", config.zone);
}

// Parse a `gcloud … --format json` array of ProvisionedNode-shaped objects,
// skipping anything malformed at the validated boundary. A production binding
// projects `gcloud compute instance-groups managed list-instances` output into this
// shape (instance → { node_id, address, capability }) via a `--format` projection;
// the reference binding consumes that shape directly so its parsing stays aligned
// with ProvisionedNode::from_json.
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

// The startup-script KEY=VALUE lines the bootstrap reads: the seed address and,
// on a secure cluster, the join token (#670). Empty when there is nothing to hand
// over.
std::string startup_script(const GcpConfig& config) {
    std::string startup;
    if (!config.seed_peer.empty()) {
        startup = "ZETTA_SEED_PEER=" + config.seed_peer;
    }
    if (!config.join_token.empty()) {
        if (!startup.empty()) startup += '\n';
        startup += "ZETTA_CLUSTER_TOKEN=" + config.join_token;
    }
    return startup;
}

// Build the `gcloud compute instance-templates create` argv. The template carries
// the per-node shape (machine type / image / network / service account / Spot /
// startup-script seed) — the MIG below just instantiates it. Spot/preemptible is a
// template property on GCP (unlike Azure VMSS, where it's a create flag), which is
// why it lands here.
//
// `startup_file`, when set, is a private file holding the startup script: it is
// passed with `--metadata-from-file` so the join token is never an argv value
// (#770). Without it the (token-free) script rides `--metadata` inline.
std::vector<std::string> template_argv(const GcpConfig& config,
                                       const SecretFile* startup_file) {
    std::vector<std::string> create{"gcloud", "compute", "instance-templates",
                                    "create", config.instance_template};
    flag(create, "--project", config.project_id);
    if (!config.machine_type.empty()) flag(create, "--machine-type", config.machine_type);
    if (!config.image.empty()) flag(create, "--image-family", config.image);
    if (!config.network.empty()) flag(create, "--network", config.network);
    if (!config.subnetwork.empty()) flag(create, "--subnet", config.subnetwork);
    if (!config.service_account.empty()) {
        flag(create, "--service-account", config.service_account);
    }
    if (config.spot) {
        flag(create, "--provisioning-model", "SPOT");
        flag(create, "--instance-termination-action", "STOP");
    }
    // Hand each node the cluster seed address at boot so it auto-joins — and, on a
    // secure cluster, the join token (#670) — via a startup-script metadata entry,
    // so the node comes up with the equivalent of `zetta --peer <seed>
    // --join-token <token>`. Instance metadata is readable by any principal that
    // can describe the template or instance, and by every process on the VM: see
    // docs/cluster/security.md for the trade-off and the Secret Manager follow-up.
    if (startup_file != nullptr) {
        flag(create, "--metadata-from-file",
             "startup-script=" + startup_file->path().string());
    } else if (const std::string startup = startup_script(config); !startup.empty()) {
        flag(create, "--metadata", "startup-script=" + startup);
    }
    return create;
}

}  // namespace

GcpCliControlPlane::GcpCliControlPlane(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

ProvisionResult GcpCliControlPlane::bring_up(const GcpConfig& config,
                                             const ProvisionRequest& request) {
    ProvisionResult result;

    // 1) Ensure the instance template exists. Templates are immutable + reused
    //    across bursts, so an "already exists" failure is the steady state — this
    //    is an idempotent ensure, not a gate (a genuine bad template makes the MIG
    //    create/resize below fail, which IS the gate).
    std::unique_ptr<SecretFile> startup_file;
    if (!config.join_token.empty()) {
        std::string reason;
        startup_file = SecretFile::create(startup_script(config), &reason);
        if (!startup_file) {
            result.satisfied = false;
            result.detail = "cannot stage the startup-script: " + reason;
            return result;
        }
    }
    runner_(template_argv(config, startup_file.get()));

    // 2) Create the MIG from the template, then 3) resize it to the requested
    //    count. Create-or-resize is what makes a re-burst idempotent on the stable
    //    pool name: a fresh MIG is created (create succeeds), an existing one is
    //    scaled (create fails "already exists", resize succeeds).
    std::vector<std::string> create{"gcloud", "compute", "instance-groups",
                                    "managed", "create", config.pool_name};
    append_pool(create, config);
    flag(create, "--template", config.instance_template);
    flag(create, "--size", std::to_string(request.count));
    const CommandResult created = runner_(create);

    std::vector<std::string> resize{"gcloud", "compute", "instance-groups",
                                    "managed", "resize", config.pool_name};
    append_pool(resize, config);
    flag(resize, "--size", std::to_string(request.count));
    const CommandResult resized = runner_(resize);

    // The pool is ready if either bringing it into existence (create) or scaling an
    // existing one (resize) succeeded. If both failed, the burst could not reach N.
    if (created.exit_code != 0 && resized.exit_code != 0) {
        result.satisfied = false;
        const std::string& reason =
            !resized.stderr_text.empty() ? resized.stderr_text
            : !created.stderr_text.empty() ? created.stderr_text
            : !resized.stdout_text.empty() ? resized.stdout_text
                                           : created.stdout_text;
        result.detail = "gcloud instance-groups managed create/resize failed: " + reason;
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

bool GcpCliControlPlane::destroy(const GcpConfig& config,
                                 const std::string& node_id) {
    std::vector<std::string> remove{"gcloud", "compute", "instance-groups",
                                    "managed", "delete-instances", config.pool_name};
    append_pool(remove, config);
    flag(remove, "--instances", node_id);
    remove.push_back("--quiet");  // non-interactive: never prompt for confirmation
    return runner_(remove).exit_code == 0;
}

std::vector<ProvisionedNode> GcpCliControlPlane::list(
    const GcpConfig& config) const {
    std::vector<std::string> query{"gcloud", "compute", "instance-groups",
                                   "managed", "list-instances", config.pool_name};
    append_pool(query, config);
    flag(query, "--format", "json");

    const CommandResult listed = runner_(query);
    if (listed.exit_code != 0) return {};
    return parse_nodes(listed.stdout_text);
}

}  // namespace zetta::cloud