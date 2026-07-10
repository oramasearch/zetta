# Node bootstrap from provisioning env

**Symbols:** `zetta::cloud::NodeBootstrap`, `BootstrapEnvApplied`,
`apply_bootstrap_env`, `split_seed_peers`, `EnvReader`
(`include/zetta/cloud/node_bootstrap_env.hpp`,
`src/core/cloud/node_bootstrap_env.cpp`) · **Issue:** #749 · **Related:** #670,
PR #742 (the injection side), `docs/plans/cloud-provisioning.md` (Gap A).

## What it does

A burst / provisioned node is handed its cluster coordinates as **environment
variables** by the provisioner seam — the control planes inject
`ZETTA_SEED_PEER` as user-data / metadata / a container env
(`src/core/cloud/*_cli_control_plane.cpp`). Until #749 the binary read no env on
boot (`grep getenv src/main.cpp` was empty), so nothing consumed them: the
env→flag bridge was an external node-bootstrap unit that did not exist (Gap A).

`main.cpp` now reads these on boot and folds them into the same node config the
CLI flags populate, **so a provisioned node auto-joins a secure (mTLS) cluster
with no manual flags** (issue #749 acceptance).

| Env var | Maps to | Effect |
|---|---|---|
| `ZETTA_SEED_PEER` | `--peer` (repeatable) | Appended to `cluster.seeds`. May list several `host:port` seeds separated by commas and/or whitespace (mirrors repeated `--peer`). |
| `ZETTA_CLUSTER_TOKEN` | `--join-token` | Sets the cluster join token (same path as `--join-token` → `load_or_create_cluster_security`), so the node derives the cluster CA + its mTLS identity and joins the secure cluster. |
| `ZETTA_DATA_DIR` | `--data-dir` | Sets the on-disk database directory. |

## Precedence — explicit flag > env > default

Env is applied as a **default beneath the flags already parsed**. `apply_bootstrap_env`
fills only the fields the operator left unset:

- `ZETTA_SEED_PEER` is used only when **no** `--peer` was given.
- `ZETTA_CLUSTER_TOKEN` is used only when **no** `--join-token` was given **and**
  the node is not running `--cluster-plaintext` — **env never forces security
  on**. A plaintext cluster (no token flag, no token env) still boots plaintext.
  The existing `--join-token` + `--cluster-plaintext` conflict check still guards
  the result.
- `ZETTA_DATA_DIR` is used only when **no** `--data-dir` was given.

Blank / unset env vars are ignored. A malformed `ZETTA_CLUSTER_TOKEN` is passed
through unvalidated exactly as `--join-token` is, so it fails loudly in
`load_or_create_cluster_security` rather than silently.

At startup, one line names what env contributed, with the token **redacted**:

```
zetta: applied provisioning env: ZETTA_SEED_PEER=10.0.0.4:7777 (--peer) ZETTA_CLUSTER_TOKEN=**** (--join-token)
```

## Testing

`apply_bootstrap_env` takes a pluggable `EnvReader` (defaulting to `std::getenv`),
so the mapping is unit-tested without touching the process environment —
`tests/cloud/test_node_bootstrap_env.cpp` proves env fills seeds + token, that a
CLI flag overrides env, and that a plaintext cluster ignores the token env.

## Not in scope (see the plan)

- **`ZETTA_ADVERTISE`.** There is no `--advertise` flag yet; a routable advertise
  address is Gap B in `docs/plans/cloud-provisioning.md` (public IP / overlay
  mesh), tracked separately.
- **Delivering + starting the binary** on the VM (a baked image / cloud-init that
  installs zetta, a Linux x86-64 artifact) is the remaining half of Gap A. Option
  (a) — the binary reads env directly — means that bootstrap unit only needs to
  install + launch the binary; it no longer has to translate env into flags, since
  the launched process inherits the injected env and consumes it itself.
