#include "zetta/cloud/k8s_cli_control_plane.hpp"

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

// The kubeconfig scope every `kubectl` call shares: the namespace, and the context
// when the config pins one (otherwise the current context). Shared by all calls.
void append_scope(std::vector<std::string>& argv, const K8sConfig& config) {
    if (!config.namespace_name.empty()) flag(argv, "--namespace", config.namespace_name);
    if (!config.context.empty()) flag(argv, "--context", config.context);
}

// `deployment/<name>` — the target form `kubectl scale` / `set` / etc. take.
std::string deployment_ref(const K8sConfig& config) {
    return "deployment/" + config.deployment_name;
}

// Parse a `kubectl get … --output json` listing of ProvisionedNode-shaped objects,
// skipping anything malformed at the validated boundary. `kubectl get` wraps a list
// in a top-level { "items": [ ... ] } envelope, so accept that as well as a bare
// array. A production binding projects `kubectl get pods` output into the
// { node_id, address, capability } shape (pod → name + IP); the reference binding
// consumes that shape directly so its parsing stays aligned with
// ProvisionedNode::from_json.
std::vector<ProvisionedNode> parse_nodes(const std::string& json_text) {
    std::vector<ProvisionedNode> nodes;
    nlohmann::json document =
        nlohmann::json::parse(json_text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) return nodes;
    if (document.is_object() && document.contains("items")) document = document["items"];
    if (!document.is_array()) return nodes;
    for (const nlohmann::json& element : document) {
        if (std::optional<ProvisionedNode> node = ProvisionedNode::from_json(element)) {
            nodes.push_back(std::move(*node));
        }
    }
    return nodes;
}

}  // namespace

std::string cluster_token_secret_name(const K8sConfig& config) {
    return config.deployment_name + "-cluster-token";
}

K8sCliControlPlane::K8sCliControlPlane(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

ProvisionResult K8sCliControlPlane::bring_up(const K8sConfig& config,
                                             const ProvisionRequest& request) {
    ProvisionResult result;

    // 1) Ensure the Deployment exists. The stable name makes this idempotent — an
    //    "already exists" failure is the steady state, so this is an ensure (not a
    //    gate; the scale below is the gate).
    std::vector<std::string> create{"kubectl", "create", "deployment",
                                    config.deployment_name};
    flag(create, "--image", config.image);
    append_scope(create, config);
    runner_(create);

    // 2) Apply the per-pod shape onto the Deployment's template. Each `kubectl set`
    //    / `patch` triggers a rollout, so the new pods carry it.
    //    Hand each pod the cluster seed address as an env var so it auto-joins —
    //    and, on a secure cluster, the join token (#670) — so the pod comes up
    //    with the equivalent of `zetta --peer <seed> --join-token <token>`.
    if (!config.seed_peer.empty()) {
        std::vector<std::string> env{"kubectl", "set", "env", deployment_ref(config),
                                     "ZETTA_SEED_PEER=" + config.seed_peer};
        append_scope(env, config);
        runner_(env);
    }
    // The cluster's identity root public key (#1343): a literal env var like the
    // seed (it is not a secret), so each pod pins the cluster's root and honors
    // its accounts instead of making a root of its own.
    if (!config.identity_root.empty()) {
        std::vector<std::string> env{"kubectl", "set", "env", deployment_ref(config),
                                     "ZETTA_IDENTITY_ROOT=" + config.identity_root};
        append_scope(env, config);
        runner_(env);
    }
    if (!config.join_token.empty()) {
        // The token lives in a Kubernetes Secret, never as a literal in the
        // Deployment spec (readable by anyone who can get Deployments) and never
        // on kubectl's argv (#770). The Secret manifest goes to `kubectl apply`
        // through a private file — apply makes it idempotent and carries a
        // re-keyed token — and the Deployment references it with `set env
        // --from=secret/…`, which writes a secretKeyRef, not the value.
        const std::string secret_name = cluster_token_secret_name(config);
        const nlohmann::json manifest = {
            {"apiVersion", "v1"},
            {"kind", "Secret"},
            {"metadata", {{"name", secret_name}}},
            {"type", "Opaque"},
            {"stringData", {{"ZETTA_CLUSTER_TOKEN", config.join_token}}}};
        std::string reason;
        const std::unique_ptr<SecretFile> manifest_file =
            SecretFile::create(manifest.dump(), &reason);
        if (!manifest_file) {
            result.satisfied = false;
            result.detail = "cannot stage the cluster join token Secret: " + reason;
            return result;
        }
        // Server-side apply: client-side apply would copy the whole manifest,
        // token included, into the last-applied-configuration annotation, which
        // `kubectl describe` prints.
        std::vector<std::string> apply{"kubectl", "apply", "--server-side"};
        flag(apply, "--field-manager", "zetta");
        flag(apply, "--filename", manifest_file->path().string());
        append_scope(apply, config);
        const CommandResult applied = runner_(apply);
        if (applied.exit_code != 0) {
            // Fail closed: pods without the token cannot join the cluster.
            result.satisfied = false;
            result.detail = "kubectl apply (cluster join token Secret) failed: " +
                            (applied.stderr_text.empty() ? applied.stdout_text
                                                         : applied.stderr_text);
            return result;
        }
        std::vector<std::string> env{"kubectl", "set", "env", deployment_ref(config),
                                     "--from=secret/" + secret_name};
        append_scope(env, config);
        runner_(env);
    }
    if (!config.service_account.empty()) {
        std::vector<std::string> sa{"kubectl", "set", "serviceaccount",
                                    deployment_ref(config), config.service_account};
        append_scope(sa, config);
        runner_(sa);
    }
    if (config.spot) {
        // Schedule onto spot/preemptible nodes via a nodeSelector. The label is
        // illustrative — production uses the cluster's actual spot node-pool label
        // (e.g. cloud.google.com/gke-spot, eks.amazonaws.com/capacityType).
        std::vector<std::string> patch{"kubectl", "patch", "deployment",
                                       config.deployment_name, "--type", "merge"};
        flag(patch, "--patch",
             R"({"spec":{"template":{"spec":{"nodeSelector":{"zetta.io/capacity":"spot"}}}}})");
        append_scope(patch, config);
        runner_(patch);
    }

    // 3) Scale the Deployment to the requested count — the authoritative, idempotent
    //    burst step (this is the gate).
    std::vector<std::string> scale{"kubectl", "scale", deployment_ref(config)};
    flag(scale, "--replicas", std::to_string(request.count));
    append_scope(scale, config);
    const CommandResult scaled = runner_(scale);
    if (scaled.exit_code != 0) {
        result.satisfied = false;
        result.detail = "kubectl scale failed: " +
                        (scaled.stderr_text.empty() ? scaled.stdout_text
                                                    : scaled.stderr_text);
        return result;
    }

    // 4) The Deployment is scaling; list what is up and report best-effort against
    //    the requested count.
    result.nodes = list(config);
    result.satisfied = result.nodes.size() >= request.count;
    if (result.satisfied) {
        result.detail = "burst deployment '" + config.deployment_name +
                        "' scaled to " + std::to_string(request.count) + " pod(s)";
    } else {
        result.detail = "best-effort: " + std::to_string(result.nodes.size()) +
                        " of " + std::to_string(request.count) +
                        " requested pods up in deployment '" +
                        config.deployment_name + "'";
    }
    return result;
}

bool K8sCliControlPlane::destroy(const K8sConfig& config,
                                 const std::string& node_id) {
    std::vector<std::string> remove{"kubectl", "delete", "pod", node_id};
    append_scope(remove, config);
    return runner_(remove).exit_code == 0;
}

std::vector<ProvisionedNode> K8sCliControlPlane::list(
    const K8sConfig& config) const {
    std::vector<std::string> query{"kubectl", "get", "pods"};
    flag(query, "--selector", "app=" + config.deployment_name);
    flag(query, "--output", "json");
    append_scope(query, config);

    const CommandResult listed = runner_(query);
    if (listed.exit_code != 0) return {};
    return parse_nodes(listed.stdout_text);
}

}  // namespace zetta::cloud