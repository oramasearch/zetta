#include "zetta/cloud/gcp_cli_control_plane.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/command_runner.hpp"
#include "zetta/cloud/node_bootstrap_script.hpp"
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

// The startup-script each node runs at boot: the node bootstrap script
// (node_bootstrap_script.hpp), carrying the cluster seed address and, on a
// secure cluster, only the *name* of the Secret Manager secret holding the join
// token (#1316). Empty when there is nothing to hand over; nullopt (with
// `reason`) when the script cannot be rendered.
std::optional<std::string> startup_script(const GcpConfig& config,
                                          std::string* reason) {
    NodeBootstrapScriptOptions options;
    options.seed_peer = config.seed_peer;
    if (!config.join_token.empty()) {
        options.token.store = ClusterTokenStore::gcp_secret_manager;
        options.token.secret_name = cluster_token_secret_name(config.pool_name);
        options.token.container = config.project_id;
    } else if (config.seed_peer.empty()) {
        return std::string{};
    }
    return render_node_bootstrap_script(options, reason);
}

// Store the join token as the Secret Manager secret `<pool>-cluster-token`:
// create it on the first burst, add a new version on every later one (so a
// rotated token reaches new nodes). The value reaches `gcloud` through a private
// file (`--data-file`), never argv (#770).
bool store_join_token(const CommandRunner& runner, const GcpConfig& config,
                      std::string* reason) {
    std::string stage_reason;
    const std::unique_ptr<SecretFile> token_file =
        SecretFile::create(config.join_token, &stage_reason);
    if (!token_file) {
        *reason = "cannot stage the cluster join token for Secret Manager: " + stage_reason;
        return false;
    }
    const std::string secret = cluster_token_secret_name(config.pool_name);
    std::vector<std::string> create{"gcloud", "secrets", "create", secret};
    flag(create, "--project", config.project_id);
    flag(create, "--replication-policy", "automatic");
    flag(create, "--data-file", token_file->path().string());
    if (runner(create).exit_code == 0) return true;

    // Already there (a re-burst): add the token as its latest version.
    std::vector<std::string> add{"gcloud", "secrets", "versions", "add", secret};
    flag(add, "--project", config.project_id);
    flag(add, "--data-file", token_file->path().string());
    const CommandResult added = runner(add);
    if (added.exit_code != 0) {
        *reason = "gcloud secrets create / versions add (cluster join token) failed: " +
                  (added.stderr_text.empty() ? added.stdout_text : added.stderr_text);
        return false;
    }
    return true;
}

// Build the `gcloud compute instance-templates create` argv. The template carries
// the per-node shape (machine type / image / network / service account / Spot /
// startup-script seed) — the MIG below just instantiates it. Spot/preemptible is a
// template property on GCP (unlike Azure VMSS, where it's a create flag), which is
// why it lands here.
//
// `startup_file`, when set, holds the startup script. It goes in with
// `--metadata-from-file` (inline `--metadata` splits its value on commas). The
// script holds no secret (#1316).
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
    // On a secure cluster the node reads the join token from Secret Manager with
    // its service account's access token, which needs the cloud-platform scope
    // (IAM, not the scope, then decides what it may read).
    if (!config.join_token.empty()) flag(create, "--scopes", "cloud-platform");
    // Hand each node its boot script as startup-script metadata: the cluster seed
    // address and, on a secure cluster, the name of the Secret Manager secret
    // holding the join token (#670, #1316). Instance metadata is readable by any
    // principal that can describe the template or instance, and by every process
    // on the VM, so it never carries the token itself: see
    // docs/cluster/security.md.
    if (startup_file != nullptr) {
        flag(create, "--metadata-from-file",
             "startup-script=" + startup_file->path().string());
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
    //    On a secure cluster the join token goes to Secret Manager first (fail
    //    closed: a node with no fetchable token cannot join, and the token never
    //    falls back to metadata), and the template carries only its name (#1316).
    if (!config.join_token.empty()) {
        if (std::string reason; !store_join_token(runner_, config, &reason)) {
            result.satisfied = false;
            result.detail = reason;
            return result;
        }
    }
    std::string render_reason;
    const std::optional<std::string> startup = startup_script(config, &render_reason);
    if (!startup) {
        result.satisfied = false;
        result.detail = "cannot render the node bootstrap script: " + render_reason;
        return result;
    }
    std::unique_ptr<SecretFile> startup_file;
    if (!startup->empty()) {
        std::string reason;
        startup_file = SecretFile::create(*startup, &reason);
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