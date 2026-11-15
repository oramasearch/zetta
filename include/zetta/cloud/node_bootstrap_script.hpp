#pragma once

// The boot script a burst VM runs to join the cluster (#1316).
//
// The burst control planes hand each VM its cluster coordinates through instance
// user-data: Azure cloud-init custom-data, the GCP `startup-script` metadata, the
// OCI instance configuration's `user_data`, an AWS launch template's user-data.
// User-data is not a secret store: every process on the VM can read it from the
// metadata endpoint, and on Azure it persists on disk, on GCP and OCI any
// principal that can describe the template / instance configuration can read it.
//
// So the cluster join token never rides user-data. The control plane stores it in
// the vendor's secret store (Key Vault, Secret Manager, OCI Vault, SSM Parameter
// Store) and the user-data is this script, which carries only the secret's
// **name**. At boot the script fetches the token with the VM's own identity
// (managed identity, service account, instance principal, instance profile),
// writes it to `/etc/zetta/cluster-token` (mode 0400, owned by the `zetta` user),
// writes `/etc/zetta/bootstrap.env` with `ZETTA_SEED_PEER`, the public
// `ZETTA_IDENTITY_ROOT` and `ZETTA_CLUSTER_CA` when set, and
// `ZETTA_CLUSTER_TOKEN_FILE`, and (re)starts `zetta.service` when the image has
// one. The engine reads the file through `ZETTA_CLUSTER_TOKEN_FILE`
// (node_bootstrap_env.hpp, #770).

#include <optional>
#include <string>
#include <string_view>

namespace zetta::cloud {

// Where the VM fetches the cluster join token from.
enum class ClusterTokenStore {
    none,                // no token to fetch (an unsecured test / embedded cluster)
    aws_ssm_parameter,   // SSM Parameter Store SecureString, read with the instance profile
    azure_key_vault,     // Key Vault secret, read with a user-assigned managed identity
    gcp_secret_manager,  // Secret Manager secret, read with the instance's service account
    oci_vault,           // OCI Vault secret, read with the instance principal
};

// The name of the secret holding the token, and what the VM needs to reach it.
// Nothing here is secret.
struct ClusterTokenReference {
    ClusterTokenStore store = ClusterTokenStore::none;
    // The SSM parameter name / Key Vault secret name / Secret Manager secret id /
    // OCI Vault secret name.
    std::string secret_name;
    // Which store holds it: the Key Vault name, the GCP project id, the OCI vault
    // OCID. Empty for AWS.
    std::string container;
    // Azure only: the resource id of the user-assigned managed identity the VM
    // reads the secret with.
    std::string identity;
    // AWS / OCI: the region of the store. Empty lets the CLI decide.
    std::string region;
};

// Where the script writes the token and the environment file the engine's
// systemd unit reads (`EnvironmentFile=`).
inline constexpr std::string_view kNodeClusterTokenPath = "/etc/zetta/cluster-token";
inline constexpr std::string_view kNodeBootstrapEnvPath = "/etc/zetta/bootstrap.env";

struct NodeBootstrapScriptOptions {
    std::string seed_peer;  // cluster seed host:port(s), written as ZETTA_SEED_PEER
    // The cluster's identity root public key (hex, not a secret), written as
    // ZETTA_IDENTITY_ROOT so the node pins the cluster's root (#1343).
    std::string identity_root;
    // The cluster's enrollment CA public key (64 lowercase hex, not a secret),
    // written as ZETTA_CLUSTER_CA so a node that is not yet enrolled enrolls only
    // through a member the CA signed (#1513, zetta#1514). Empty on a cluster
    // without an enrollment CA.
    std::string cluster_ca;
    ClusterTokenReference token;
    // The control planes keep the defaults; a test points them at a temp dir.
    std::string token_path{kNodeClusterTokenPath};
    std::string env_path{kNodeBootstrapEnvPath};
    // How often the script tries to read the token, and the pause between tries
    // (an IAM grant on a fresh identity can take minutes to propagate).
    unsigned attempts = 60;
    unsigned retry_delay_seconds = 10;
};

// The name the burst control planes store the token under in Key Vault, Secret
// Manager and OCI Vault: `<pool>-cluster-token`. (AWS uses the SSM path
// `/zetta/<pool>/cluster-token`, aws_cli_control_plane.hpp.)
std::string cluster_token_secret_name(std::string_view pool_name);

// Render the POSIX `sh` boot script. Every value is shell-quoted. Returns nullopt
// with `reason` set when a value holds a control character (it cannot be written
// into a script safely), when a store is named without the fields it needs, or
// when the identity root or cluster CA is not 64 lowercase hex characters.
std::optional<std::string> render_node_bootstrap_script(
    const NodeBootstrapScriptOptions& options, std::string* reason = nullptr);

// Quote `value` for a POSIX shell: returned as-is when it holds only characters
// that need no quoting, otherwise wrapped in single quotes with each `'` written
// as `'\''`.
std::string shell_quote(std::string_view value);

}  // namespace zetta::cloud
