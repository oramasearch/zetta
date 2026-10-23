# Node bootstrap from provisioning env

**Symbols:** `zetta::cloud::NodeBootstrap`, `BootstrapEnvApplied`,
`apply_bootstrap_env`, `scrub_bootstrap_secrets`, `split_seed_peers`, `EnvReader`
(`include/zetta/cloud/node_bootstrap_env.hpp`,
`src/core/cloud/node_bootstrap_env.cpp`) · **Issues:** #749, #770 · **Related:** #670,
PR #742 (the injection side), `docs/plans/cloud-provisioning.md` (Gap A).

## What it does

A burst / provisioned node is handed its cluster coordinates as **environment
variables** by the provisioner interface — the control planes inject
`ZETTA_SEED_PEER` as user-data / metadata / a container env
(`src/core/cloud/*_cli_control_plane.cpp`), and the join token as described in
[cluster security](../cluster/security.md#how-the-token-is-delivered-per-provider-770). Until #749 the binary read no env on
boot (`grep getenv src/main.cpp` was empty), so nothing consumed them: the
env→flag bridge was an external node-bootstrap unit that did not exist (Gap A).

`main.cpp` now reads these on boot and folds them into the same node config the
CLI flags populate, **so a provisioned node auto-joins a secure (mTLS) cluster
with no manual flags** (issue #749 acceptance).

| Env var | Maps to | Effect |
|---|---|---|
| `ZETTA_SEED_PEER` | `--peer` (repeatable) | Appended to `cluster.seeds`. May list several `host:port` seeds separated by commas and/or whitespace (mirrors repeated `--peer`). |
| `ZETTA_CLUSTER_TOKEN_FILE` | `--join-token` | **Preferred.** Path of a file holding the join token (a Kubernetes Secret volume, a mode-0400 file). Its trimmed contents set the token, so the value never sits in the process environment. |
| `ZETTA_CLUSTER_TOKEN` | `--join-token` | Sets the cluster join token (same path as `--join-token` → `load_or_create_cluster_security`), so the node derives the cluster CA + its mTLS identity and joins the secure cluster. Used when no token file is named. |
| `ZETTA_DATA_DIR` | `--data-dir` | Sets the on-disk database directory. |

## Precedence — explicit flag > env > default

Env is applied as a **default beneath the flags already parsed**. `apply_bootstrap_env`
fills only the fields the operator left unset:

- `ZETTA_SEED_PEER` is used only when **no** `--peer` was given.
- `ZETTA_CLUSTER_TOKEN_FILE` / `ZETTA_CLUSTER_TOKEN` are used only when **no**
  `--join-token` was given; the file wins over the variable.
  Inter-node mTLS is always on (#799 — the `--cluster-plaintext` opt-out was
  removed), so a node always needs a join token: an explicit `--join-token` wins,
  otherwise this env supplies it.
- `ZETTA_DATA_DIR` is used only when **no** `--data-dir` was given.

Blank / unset env vars are ignored.

At startup, one line names what env contributed, with the token **redacted** and
its source named:

```
zetta: applied provisioning env: ZETTA_SEED_PEER=10.0.0.4:7777 (--peer) ZETTA_CLUSTER_TOKEN_FILE (/run/secrets/zetta/token)=**** (--join-token; joins the cluster this token identifies)
```

## The join token (#770)

An environment variable is a wide place to keep a secret: it is readable through
`/proc/<pid>/environ` (same user or root) and `ps eww`, shown by `docker inspect`,
and **inherited by every child process** — and the node runs children (the cloud
CLIs, container runtimes, the apps it hosts). So:

- **Prefer `ZETTA_CLUSTER_TOKEN_FILE`.** The node reads the file at boot. It must
  be a regular file of at most 4096 bytes; a missing, unreadable, oversized or
  empty file **refuses the start** with an error naming the path. A file other
  users can read (an `others` read bit) starts, with a warning to restrict it to
  `0400` / `0440`.
- **Trimmed and validated at the source.** Surrounding whitespace is removed (a
  token file usually ends in a newline). A value that is blank after trimming is
  treated as unset. A token that is not 64 lowercase hex refuses the start with an
  error that names where it came from (`the join token from ZETTA_CLUSTER_TOKEN
  must be …`), never the value, and never as a `--join-token` that was not typed.
- **One token.** When both the file and the variable are set they must hold the
  same token (an operator moving from one to the other sets both for a while);
  different tokens refuse the start rather than pick one silently.
- **Scrubbed after reading.** `scrub_bootstrap_secrets()` removes
  `ZETTA_CLUSTER_TOKEN` from the node's environment as soon as it is read, so no
  child process inherits it. The kernel's copy of the *initial* environment
  (`/proc/<pid>/environ`) is not changed by that, which is why the file is
  preferred.

**An inherited token joins its cluster.** A node started without `--join-token`
adopts any valid `ZETTA_CLUSTER_TOKEN` (or token file) it finds in its
environment: an inherited shell, a systemd `EnvironmentFile`, a CI runner, stale
provisioning. It then joins the cluster that token identifies instead of founding
its own. The startup line says so and names the source. To found a new cluster on
a host that has one set, unset it (or pass `--join-token` explicitly).

## Testing

`apply_bootstrap_env` takes a pluggable `EnvReader` (defaulting to `std::getenv`),
so the mapping is unit-tested without touching the process environment —
`tests/cloud/test_node_bootstrap_env.cpp` proves env fills seeds + token, that a
CLI flag overrides env, and (#770) that the token is trimmed, that a malformed or
whitespace-only token is rejected or ignored with its source named, that the token
file is read and preferred, that a missing / empty / malformed file or a file and
variable that disagree refuse the start, that a world-readable file warns, and that
`scrub_bootstrap_secrets` removes the variable from the process environment.

## Not in scope (see the plan)

- **`ZETTA_ADVERTISE`.** There is no `--advertise` flag yet; a routable advertise
  address is Gap B in `docs/plans/cloud-provisioning.md` (public IP / overlay
  mesh), tracked separately.
- **The VM side of the token file** is the
  [node bootstrap script](node-bootstrap-script.md) (#1316): it fetches the token
  from the vendor secret store at boot, writes it to a 0400 file and points
  `ZETTA_CLUSTER_TOKEN_FILE` at it through `/etc/zetta/bootstrap.env`.
- **Delivering + starting the binary** on the VM (a baked image / cloud-init that
  installs zetta, a Linux x86-64 artifact) is the remaining half of Gap A. Option
  (a) — the binary reads env directly — means that bootstrap unit only needs to
  install + launch the binary; it no longer has to translate env into flags, since
  the launched process inherits the injected env and consumes it itself.
