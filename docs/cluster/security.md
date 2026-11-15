# Cluster transport security — mTLS + per-node identity + the join token

Blueprint **CTL-2** for trust boundary **TB-3** (the inter-node boundary), #466:
every inter-node connection runs **mutual TLS 1.3**, every node holds a
**cryptographic identity** (its own keypair + certificate), and joining the
cluster is **gated on a join token**. Built on Corosio's OpenSSL TLS provider
over the same equal-peer transport ([cluster.md](cluster.md)); generic TLS and
framing live in [`zetta::transport`](transport-library.md), while the
cluster-specific surface is `zetta::cluster::ClusterSecurity` and
`ClusterConfig::security`.

## Trust model — the join token is the trust root

A cluster has one **join token**: 32 CSPRNG bytes, printed as 64 lowercase hex
characters. Everything else derives from it:

1. **The cluster CA is derived deterministically from the token.** An Ed25519
   private key *is* a 32-byte seed, so `SHA256(token-bytes ‖ "zetta-cluster-ca-v1")`
   gives every token holder the **same CA keypair** — and, with all certificate
   fields fixed (subject `CN=zetta-cluster-ca`, a fixed 2026→2076 validity
   window, serial from the subject hash), the same CA certificate. No CA ever
   travels over the network and no enrollment RPC exists: **possession of the
   token is the ability to mint a cluster-trusted identity**, which matches the
   equal-peer, no-leader architecture (any member could enroll others anyway).
2. **Each node mints its own identity**: a fresh random Ed25519 keypair in a
   CA-signed certificate whose `CN` is the node id, persisted next to the token
   (below) so the identity is stable across restarts.
3. **The transport enforces it**: `Cluster::start()` asks
   `zetta::transport::make_mutual_tls_context` to verify the PEM bundle and
   build one context — this node's identity + the cluster CA as the **only**
   trust anchor, `require_peer` verification in **both** directions, TLS 1.3
   only, no
   hostname check (membership is proven by the CA, not by an address). The TLS
   handshake runs **before** any `ZETTA/1` protocol line, so an unenrolled or
   plaintext dialer is dropped before it even reaches `HELLO`. Credentials that
   fail to load **fail closed** — the node runs no networking rather than
   falling back to plaintext.
4. **The `HELLO` node id is pinned to the certificate** (#669). After the
   handshake, each side reads the certificate its peer presented
   (`FramedStream::peer_certificate_der()`) and refuses the link unless the node
   id in the peer's `HELLO` is that certificate's subject common name
   (`hello_matches_peer_certificate` in `cluster.cpp`). The refusal happens
   before the peer is registered, recorded or gossiped with, and prints one line
   to stderr that echoes neither the stated id nor the certificate's name. Every
   node's certificate already carries its node id as `CN` (step 2), so this
   needs no wire or on-disk change and nodes from earlier versions connect
   unchanged.
5. **One shared framed TLS stream, full duplex.** A link's read pump and write
   pump share one `zetta::transport::FramedStream` over Corosio's
   `openssl_stream`. Its `SSL` calls never suspend, so a read and a write never
   drive the one `SSL` object at the same time (#786), and its socket reads and
   socket writes take separate locks, so a read waiting for the peer never holds
   back a write and a write waiting for socket space never holds back a read.
   That second property is what keeps a link alive when both peers send a large
   frame at once (gossip re-ships the full document each round; incremental sync
   is #794): with one lock for both directions, two peers could each park their
   reader under the lock waiting for the other, and the link froze silently
   (#835). See [transport-library.md](transport-library.md).

Mixed modes fail cleanly in both directions: a TLS node treats a plaintext
`HELLO` as a failed handshake; a plaintext node treats a TLS `ClientHello` as a
malformed line. Either way the connection drops and nothing joins.

## Joining a cluster

The first node generates the token on first start and prints where it is and
the command to join, with the token masked:

```bash
# First node — generates data_dir/security/, prints:
#   zetta: inter-node mTLS on (join token in .../security/cluster-secret.json)
#   zetta: add another node with: zetta --peer 192.168.1.10:7777 --join-token <join-token> # Another machine joins with that command, the token read from the file above (or passed as ZETTA_CLUSTER_TOKEN): <64-hex> --identity-root <64-hex> exact command:
zetta --peer 192.168.1.10:7777 --join-token <64-hex> --identity-root <64-hex>

```
**The token is never printed or logged (#1393).** It derives the cluster CA key
and is the connector-credential sealing root (#1230), so anyone holding it can
mint a trusted peer identity. `Cluster::peer_command()` (the headless banner and
the Cluster panel's text) puts `kJoinTokenPlaceholder` (`<join-token>`) where the
token goes. Only `Cluster::peer_command_with_join_token()` returns the clear from `--join-token`, from `ZETTA_CLUSTER_TOKEN` and generated on first
token, and its one caller is the headed Cluster panel's **Copy command** button,
an explicit operator action. A refused secret file or `--join-token` names the
file and the field, never the value. The process test
`tests/cluster/test_startup_log_redaction.cpp` starts the real binary with the
start, and fails if any output line contains it. A cluster whose token reached
logs before this change should be re-keyed (pass a new `--join-token`; the old
one is kept as a retired token so sealed connector credentials still open).
The join token makes a node a **member**; it confers no authority over
identity. `--identity-root` is the public key of the cluster's identity root,
which seals every account, role and token: the joiner pins it and honors only
records sealed by it. It is not a secret. A node given the cluster's
`--admin-token` (every node under the operator) derives the same root and needs
no flag. See [identity-seal.md](../governance/identity-seal.md) (#763, #764).

`--join-token` is persisted in the joiner's own data dir on first use, so it is
needed only once. Passing a *different* token to a node that already has one
**re-keys** it: the new token wins, is persisted, and the node's identity is
re-minted under the new CA. Inter-node mTLS is **always on** — there is **no
plaintext peer mode**. The old `--cluster-plaintext` opt-out was **removed**
(#799): it disabled the whole subsystem and was a silent-downgrade security
hole. The only way to run an unsecured `Cluster` now is to embed a bare
`Cluster` with an all-empty `ClusterSecurity` in a test or host process — no CLI
flag or env selects it.

### Provisioned / burst nodes (#670)

A cloud-burst node can't be handed these flags interactively, so the provisioner
interface injects them at boot. Each `<Vendor>ControlPlane` hands the node the
cluster's seed address in **`ZETTA_SEED_PEER`** and — when the cluster is secure —
its join token, so the node's bootstrap comes up with the equivalent of
`zetta --peer <seed> --join-token <token>` and joins the **mTLS** cluster (the
only kind — inter-node mTLS is always on, #799). Both are injected once, at fleet
wire time, from this node's own live cluster security — `main.cpp` passes
`ClusterConfig::security.join_token` to `ProvisionerFleet::wire`, which fills any
per-cloud config that didn't set its own. The token is the cluster secret, so
unlike the seed peer it is **never** part of the config's JSON boundary: it is not
serialized into the node's detected provisioner config or the `system.provisioners`
table, and rides only the bootstrap path. A `Cluster` with empty security (an
embedded bare `Cluster` in a test/host, the only unsecured path — #799) has an
empty token, so nothing is injected.

The cluster's **identity root** public key rides the same path as
**`ZETTA_IDENTITY_ROOT`** (#1343): `main.cpp` passes `ClusterConfig::identity_root`
to `ProvisionerFleet::wire`, which sets it on every config (it is public, so it is a
literal env var, tag or environment-file line, never a secret store), and the burst
node pins the cluster's root instead of making its own
([identity-seal.md](../governance/identity-seal.md)).

#### How the token is delivered, per provider (#770)

**Never on a command line.** The control planes run vendor CLIs as child
processes, and a child's argv is readable by every user on the host while it
runs (`ps`, `/proc/<pid>/cmdline`). The token therefore always reaches the CLI
through a mode-0600 file in a fresh mode-0700 directory, removed when the command
returns (`zetta::cloud::SecretFile`, `include/zetta/cloud/secret_file.hpp`).

| Provider | How the node gets the token | Who else can read it |
|---|---|---|
| **Kubernetes** (`k8s_cli_control_plane.cpp`) | Secret `<deployment>-cluster-token` (key `ZETTA_CLUSTER_TOKEN`), applied with `kubectl apply --server-side` from the private file; the Deployment references it with `kubectl set env --from=secret/…` (a `secretKeyRef`). Server-side apply keeps the value out of the `last-applied-configuration` annotation that `kubectl describe` prints. | Principals that can read Secrets in the namespace; the pod's processes through their environment. No longer anyone who can read the Deployment. |
| **AWS** (`aws_cli_control_plane.cpp`) | SSM Parameter Store SecureString `/zetta/<pool>/cluster-token` (`aws ssm put-parameter --value file://…`, fail-closed); the instance carries only the **name**, in the propagating tag `ZETTA_CLUSTER_TOKEN_SSM_PARAMETER`. The node's instance profile needs `ssm:GetParameter` on it and `kms:Decrypt` on its key. | Principals granted `ssm:GetParameter` (+ decrypt). Before #770 the token was itself a tag, readable by anyone with `ec2:DescribeTags` and recorded in CloudTrail. |
| **Bare metal / containers** (`bare_metal_cli_control_plane.cpp`) | `<runtime> run --env-file <private file>`. | Anyone who can `docker inspect` the container (the runtime stores container env), and the container's processes. |
| **Azure** (`azure_cli_control_plane.cpp`) | Key Vault secret `<pool>-cluster-token` in `key_vault` (`az keyvault secret set --file …`, fail-closed; no `key_vault` / `managed_identity` → nothing is created). The custom-data is the [node bootstrap script](../cloud/node-bootstrap-script.md), carrying only the secret's name; at boot it reads the secret with the user-assigned managed identity and writes a 0400 file (#1316). | Principals with Key Vault Secrets User (or `get`) on the secret, the burst nodes' identity among them; the node's processes that can read the 0400 file. No longer every process on the VM, nor its disk. |
| **GCP** (`gcp_cli_control_plane.cpp`) | Secret Manager secret `<pool>-cluster-token` (`gcloud secrets create` / `versions add --data-file …`, fail-closed). The `startup-script` is the node bootstrap script, carrying only the secret's name; the template gets `--scopes cloud-platform`, and at boot the script reads the secret with the instance's service account (#1316). | Principals with `secretmanager.secretAccessor` on the secret, the node service account among them. No longer anyone who can describe the template or instance, nor every process on the VM. |
| **OCI** (`oci_cli_control_plane.cpp`) | OCI Vault secret `<pool>-cluster-token` in `vault_id`, encrypted with `key_id` (`oci vault secret create-base64` / `update-base64 --from-json file://…`, fail-closed; no `vault_id` / `key_id` → nothing is created). The instance configuration's `user_data` is the base64 node bootstrap script, carrying only the secret's name; at boot it reads the secret with the instance principal (#1316). | Principals allowed `read secret-bundles` on it, the burst nodes' dynamic group among them. No longer anyone who can read the instance configuration, nor every process on the instance. |

No provider puts the token in instance user-data or metadata any more (#1316). The
VM's user-data is the [node bootstrap script](../cloud/node-bootstrap-script.md),
which names the secret; the VM reads it with its own identity and writes
`/etc/zetta/cluster-token` (0400, owned by `zetta`). On AWS the operator's launch
template runs the same script (#1338 generates that from the binding). Templates and
instance configurations created before #1316 still hold the token: the GCP and OCI
provider pages say how to remove them, and the token should be rotated. #669 (CSR
enrollment) would replace the long-lived token on new nodes with a one-time
enrollment credential.

**On the node**, the token arrives as `ZETTA_CLUSTER_TOKEN_FILE` (preferred — a
Kubernetes Secret volume, a 0400 file the bootstrap writes) or
`ZETTA_CLUSTER_TOKEN`. The engine trims and validates it against its source,
refuses to start when the two disagree, and removes `ZETTA_CLUSTER_TOKEN` from its
own environment once read so no child process inherits it; see
[node bootstrap](../cloud/node-bootstrap-env.md#the-join-token-770).

## On disk — `<data_dir>/security/`

The directory is created with mode **0700**, and files with mode **0600**
before any secret bytes are written, independently of the process umask
(#1413, LSR-240). Cluster identity persistence and the connector's local root
secret use the same directory-relative storage helper. Existing directories
and files are restricted before use; symlinks, multiply linked files,
non-regular files and objects owned by another UID refuse the load.
Unreadable files fail the load rather than being treated as missing.

Each write uses an exclusive private staging file and renames it over the
destination after the bytes are written and synced. An already-open descriptor
to an earlier inode cannot read a newly rotated secret. Failed writes remove
their staging file and leave the destination intact. Operators must keep the
data directory and its ancestors under trusted control; changing permissions
cannot revoke secret bytes an earlier reader already obtained.

| File | What | Mode |
|---|---|---|
| `cluster-secret.json` | `{"version": 1, "join_token": "<64 hex>", "retired_join_tokens"?: ["<64 hex>", …]}` — schema-validated on load (`cluster_secret_schema()`, generated `schemas/cluster_secret.schema.json`); a malformed file **fails the start with a reason**, it is never silently clobbered. A `--join-token` that replaces the stored token moves it to `retired_join_tokens` (newest first, at most four) | 0600 |
| `connection-root-secret` | only on a node embedded **without** a join token (the library, a test): the 64-hex root secret its saved connector credentials are sealed under (#1230) | 0600 |
| `node-cert.pem` | this node's CA-signed identity certificate (`CN` = node id) | 0600 |
| `node-key.pem` | this node's Ed25519 private key | 0600 |
| `identity-root.json` | the pinned identity root: `{"version": 1, "public_key": "<64 hex>", "seed"?: "<64 hex>"}`; `seed` only for a root the node generated (no owner token). Schema `identity_root_schema()`. See [identity-seal.md](../governance/identity-seal.md) | 0600 with a seed, else 0644 |
| `identity-sealed` | marker: the one-time sealing of identity records held before the root ran | — |
| `identity-held.json` | the newest sealed record this node accepted for each identity key: `{"version": 1, "records": {"<key>": <sealed value>}}`, so a restart still refuses an older one. Each record is verified again on load; a file that does not parse refuses the start. See [identity-seal.md](../governance/identity-seal.md#versions-and-removals-1341) (#1341) | 0644 |

The stored identity is reused while it still verifies against the derived CA
and carries this node's id; a token change or a node-id mismatch re-mints it.

The join token is also the root secret of the **connector-credential sealing
key** (#1230): `main` passes it as `NodeConfig::connection_root_secret`, and saved
connections' credentials are sealed under an HKDF-SHA256 key derived from it, so
every member opens what another sealed and the key is never in the replicated
catalogue. The retired tokens let a re-keyed node open what was sealed under the
previous one; its next start re-seals those entries under the current token. See
[connector query](../data/connector-query.md#the-credential-is-sealed-at-rest-1230).

## Code map

- `include/zetta/cluster/security.hpp` — `ClusterSecurity` (the PEM bundle +
  token), `generate_join_token` / `valid_join_token`,
  `derive_cluster_security` (pure), `load_or_create_cluster_security`
  (persistence), `cluster_secret_schema()`.
- `include/zetta/transport/` — the independent public transport surface:
  verified PEM credentials, mutual TLS 1.3 context creation, and the framed
  Corosio stream. See [transport-library.md](transport-library.md).
- `src/transport/` — the `zetta_transport` implementation: OpenSSL credential
  validation and the shared framing/TLS I/O coordination (#1283).
- `src/core/cluster/security.cpp` — the **one TU allowed to name OpenSSL
  libcrypto for cluster identity generation** (key generation, X509 minting,
  PEM). Generic PEM verification lives in `src/transport/security.cpp`.
- `src/core/cluster/cluster.cpp` — cluster connection lifecycle and protocol;
  it creates the shared context and wraps every socket in `FramedStream` before
  HELLO. The read-pump + link-writer pattern and line bytes are unchanged.
- `src/core/util/private_directory.{hpp,cpp}` — shared owner-only directory
  creation, checked reads and private file replacement for cluster and
  connector secret persistence; no JSON or wire-format changes (#1413).
- `tests/cluster/test_cluster_security.cpp` — token/derivation/persistence
  units + real-TCP mTLS convergence, wrong-token rejection, plaintext
  rejection, and the `HELLO`-to-certificate pin
  (`CertificateCommonNameIsTheNodeId`,
  `PeerWhoseHelloIsNotItsCertificateIsRefused`).

## Threat model + known limitations

**Closed** (TB-3 vs outsiders): eavesdropping and tampering on inter-node
traffic (TLS 1.3), uninvited joins (no token → no CA-signed cert → handshake
fails), silent plaintext downgrade (fail-closed credential loading, no
fallback path).

**Closed by #669 (pinning):** a peer's stated node id is bound to its
certificate. A process holding one node's key and certificate, but not the
join token, can no longer claim another node's id in its `HELLO`, and a link is
never recorded under an id its certificate does not name. This is the check the
two items below depend on.

**Still open:**

- **Member-vs-member node-id impersonation by a token holder.** The CA key is
  derivable from the join token, so any member (anyone holding the token) can
  mint a certificate whose `CN` is another node's id, and pinning accepts it.
  Closing this means CSR-based enrollment (#1319): a joining node generates its
  key locally and an existing member signs it, so the CA key lives with members
  rather than inside the token.
- **No revocation.** Removing a node's trust today means rotating the token
  (re-key every node). Token rotation is manual (`--join-token` on each node);
  each node re-seals the saved connector credentials on its first start under
  the new token. A per-node revocation list checked at every handshake (#1320)
  is only meaningful once the token no longer mints certificates (the item
  above).
**Closed by #670:** burst nodes now receive the join token too — the provisioner
interface injects `ZETTA_CLUSTER_TOKEN` alongside `ZETTA_SEED_PEER`, so a provisioned
node maps it with `ZETTA_SEED_PEER` to the CLI defaults, derives the cluster CA
and identity, and joins a secure cluster with no manual flags (see "Provisioned
/ burst nodes" above and [node bootstrap](../cloud/node-bootstrap-env.md)).
