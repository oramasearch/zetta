# Node bootstrap script — the join token from the vendor secret store

**Symbols:** `zetta::cloud::render_node_bootstrap_script`, `NodeBootstrapScriptOptions`,
`ClusterTokenReference`, `ClusterTokenStore`, `cluster_token_secret_name`, `shell_quote`
(`include/zetta/cloud/node_bootstrap_script.hpp`, `src/core/cloud/node_bootstrap_script.cpp`)
· **Issues:** #1316 (this), #770 (the token off argv, AWS SSM, Kubernetes Secret), #1338
(AWS launch template) · **Related:** [node-bootstrap-env.md](node-bootstrap-env.md),
[cluster security](../cluster/security.md#how-the-token-is-delivered-per-provider-770).

## What it is for

A burst VM gets its cluster coordinates through instance user-data: Azure cloud-init
custom-data, the GCP `startup-script` metadata, the OCI instance configuration's
`user_data`, an AWS launch template's user-data. User-data is not a secret store.
Every process on the VM can read it from the metadata endpoint; on Azure it persists
on disk (`/var/lib/waagent/CustomData`); on GCP and OCI any principal that can
describe the instance template or instance configuration can read it.

So the cluster join token never rides user-data. The control plane stores it in the
vendor's secret store, and the user-data is this script, which carries only the
secret's **name**:

| Provider | Where the token is stored | How the VM reads it |
|---|---|---|
| Azure | Key Vault secret `<pool>-cluster-token` in `key_vault` | a Key Vault access token for the user-assigned `managed_identity` from the metadata endpoint, then `GET https://<vault>.vault.azure.net/secrets/<name>` |
| GCP | Secret Manager secret `<pool>-cluster-token` in `project_id` | an access token for the instance's service account from the metadata server, then `…/secrets/<name>/versions/latest:access` |
| OCI | OCI Vault secret `<pool>-cluster-token` in `vault_id` | `oci secrets secret-bundle get-secret-bundle-by-name --auth instance_principal` |
| AWS | SSM SecureString `/zetta/<pool>/cluster-token` | `aws ssm get-parameter --with-decryption` with the instance profile |

Azure, GCP and OCI hand the script to their VMs themselves
([azure](azure-provisioner.md#the-join-token--key-vault-and-a-managed-identity-1316),
[gcp](gcp-provisioner.md#the-join-token--secret-manager-and-the-node-service-account-1316),
[oci](oci-provisioner.md#the-join-token--oci-vault-and-the-instance-principal-1316)).
On AWS the launch template is the operator's, so the operator's user-data runs it
([aws](aws-provisioner.md#the-join-token-on-the-node--the-launch-templates-user-data-1316));
generating the launch template version from the binding is #1338. Each provider page
lists the IAM the operator must grant.

## What the script does

It is POSIX `sh` (Debian and Ubuntu run user-data with `dash`), and needs `curl`
(Azure, GCP), the `oci` or `aws` CLI (OCI, AWS), `base64`, `sed` and `mktemp`.

1. **Fetch.** It reads the token with the VM's own identity, retrying (60 tries, 10
   seconds apart) while a fresh IAM grant propagates.
2. **Validate.** Whitespace is stripped and the value must be 64 lowercase hex
   characters. After the last failed try it logs which store and secret it could
   not read and exits non-zero, without writing anything or starting the engine.
3. **Write the token file.** `/etc/zetta/cluster-token`, mode 0400, owned by the
   `zetta` user (created as a system user when missing), written to a temporary
   file in the same directory and renamed into place.
4. **Write the environment file.** `/etc/zetta/bootstrap.env` (0644) holds
   `ZETTA_SEED_PEER=<seed>`, `ZETTA_IDENTITY_ROOT=<public key>` (the cluster's
   identity root, #1343) and `ZETTA_CLUSTER_TOKEN_FILE=/etc/zetta/cluster-token`,
   never the token.
5. **Start the engine.** When the image has a `zetta.service` unit it is enabled
   and restarted; otherwise the files are left for it and the script says so.

The token never appears in an argument, in the environment file or in a log line.
Access tokens from the metadata endpoints reach `curl` through a private header file
(`-H @file`), not as an argument, and every value written into the script is
shell-quoted (`shell_quote`); a value with a control character is refused when the
script is rendered.

## The engine side

The engine reads the file through `ZETTA_CLUSTER_TOKEN_FILE`
([node-bootstrap-env.md](node-bootstrap-env.md), #770), and the seed through
`ZETTA_SEED_PEER`. A unit that consumes the environment file:

```ini
[Unit]
Description=zetta node
After=network-online.target
Wants=network-online.target

[Service]
User=zetta
EnvironmentFile=/etc/zetta/bootstrap.env
ExecStart=/usr/local/bin/zetta --headless --data-dir /var/lib/zetta
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Getting the binary and this unit onto the VM (a baked image, or a cloud-init that
installs a release artifact) is the rest of Gap A in
[cloud provisioning](../plans/cloud-provisioning.md); the script works with either.

## Rotation

Every burst writes the current token to the store again (a new secret version on
Azure, GCP and OCI; an overwrite on AWS), so nodes that come up after a rotation
fetch the new one. Nodes already running keep the file they wrote at boot.

## Testing

`tests/cloud/test_node_bootstrap_script.cpp` renders the script and checks it holds
the secret name and never a token, that a value cannot break out of its quotes, and
that control characters and missing fields are refused. It then **runs** the script
under `/bin/sh` (and `/bin/dash` where present) against fake `curl`, `oci`, `aws`,
`systemctl`, `id`, `useradd` and `logger` executables on `PATH`, one fake per vendor
that answers only the expected request: for each provider the token lands in a 0400
file, the environment file names it, `zetta.service` is restarted, and no access
token or join token appears in any argument. Two more runs prove the retry while a
grant propagates and that a malformed value is never written. No cloud is called.
The control-plane tests (`test_azure_provisioner.cpp`, `test_gcp_provisioner.cpp`,
`test_oci_provisioner.cpp`) check the store calls, the fail-closed paths, and that
the user-data each binding sends is this script.
